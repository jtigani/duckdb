#include "duckdb/planner/subquery/flatten_dependent_join.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/subquery/has_correlated_expressions.hpp"
#include "duckdb/planner/subquery/rewrite_correlated_expressions.hpp"

namespace duckdb {

FlattenDependentJoins::FlattenDependentJoins(Binder &binder, const vector<CorrelatedColumnInfo> &correlated,
                                             bool perform_delim, bool any_join)
    : binder(binder), correlated_columns(correlated), perform_delim(perform_delim), any_join(any_join) {
	for (idx_t i = 0; i < correlated_columns.size(); i++) {
		auto &col = correlated_columns[i];
		correlated_map[col.binding] = i;
		delim_types.push_back(col.type);
	}
}

bool FlattenDependentJoins::DetectCorrelatedExpressions(LogicalOperator *op) {
	D_ASSERT(op);
	// check if this entry has correlated expressions
	HasCorrelatedExpressions visitor(correlated_columns);
	visitor.VisitOperator(*op);
	bool has_correlation = visitor.has_correlated_expressions;
	// now visit the children of this entry and check if they have correlated expressions
	for (auto &child : op->children) {
		// we OR the property with its children such that has_correlation is true if either
		// (1) this node has a correlated expression or
		// (2) one of its children has a correlated expression
		if (DetectCorrelatedExpressions(child.get())) {
			has_correlation = true;
		}
	}
	// set the entry in the map
	has_correlated_expressions[op] = has_correlation;
	return has_correlation;
}

unique_ptr<LogicalOperator> FlattenDependentJoins::PushDownDependentJoin(unique_ptr<LogicalOperator> plan) {
	bool propagate_null_values = true;
	auto result = PushDownDependentJoinInternal(move(plan), propagate_null_values);
	if (!replacement_map.empty()) {
		// check if we have to replace any COUNT aggregates into "CASE WHEN X IS NULL THEN 0 ELSE COUNT END"
		RewriteCountAggregates aggr(replacement_map);
		aggr.VisitOperator(*result);
	}
	return result;
}

bool SubqueryDependentFilter(Expression *expr) {
	if (expr->expression_class == ExpressionClass::BOUND_CONJUNCTION &&
	    expr->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto bound_conjuction = (BoundConjunctionExpression *)expr;
		for (auto &child : bound_conjuction->children) {
			if (SubqueryDependentFilter(child.get())) {
				return true;
			}
		}
	}
	if (expr->expression_class == ExpressionClass::BOUND_SUBQUERY) {
		return true;
	}
	return false;
}
unique_ptr<LogicalOperator> FlattenDependentJoins::PushDownDependentJoinInternal(unique_ptr<LogicalOperator> plan,
                                                                                 bool &parent_propagate_null_values) {
	// first check if the logical operator has correlated expressions
	auto entry = has_correlated_expressions.find(plan.get());
	D_ASSERT(entry != has_correlated_expressions.end());
	if (!entry->second) {
		// we reached a node without correlated expressions
		// we can eliminate the dependent join now and create a simple cross product
		// now create the duplicate eliminated scan for this node
		auto delim_index = binder.GenerateTableIndex();
		this->base_binding = ColumnBinding(delim_index, 0);
		auto delim_scan = make_unique<LogicalDelimGet>(delim_index, delim_types);
		return LogicalCrossProduct::Create(move(delim_scan), move(plan));
	}
	switch (plan->type) {
	case LogicalOperatorType::LOGICAL_UNNEST:
	case LogicalOperatorType::LOGICAL_FILTER: {
		// filter
		// first we flatten the dependent join in the child of the filter
		for (auto &expr : plan->expressions) {
			any_join |= SubqueryDependentFilter(expr.get());
		}
		plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);

		// then we replace any correlated expressions with the corresponding entry in the correlated_map
		RewriteCorrelatedExpressions rewriter(base_binding, correlated_map);
		rewriter.VisitOperator(*plan);
		return plan;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		// projection
		// first we flatten the dependent join in the child of the projection
		for (auto &expr : plan->expressions) {
			parent_propagate_null_values &= expr->PropagatesNullValues();
		}
		plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);

		// then we replace any correlated expressions with the corresponding entry in the correlated_map
		RewriteCorrelatedExpressions rewriter(base_binding, correlated_map);
		rewriter.VisitOperator(*plan);
		// now we add all the columns of the delim_scan to the projection list
		auto proj = (LogicalProjection *)plan.get();
		for (idx_t i = 0; i < correlated_columns.size(); i++) {
			auto &col = correlated_columns[i];
			auto colref = make_unique<BoundColumnRefExpression>(
			    col.name, col.type, ColumnBinding(base_binding.table_index, base_binding.column_index + i));
			plan->expressions.push_back(move(colref));
		}

		base_binding.table_index = proj->table_index;
		this->delim_offset = base_binding.column_index = plan->expressions.size() - correlated_columns.size();
		this->data_offset = 0;
		return plan;
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggr = (LogicalAggregate &)*plan;
		// aggregate and group by
		// first we flatten the dependent join in the child of the projection
		for (auto &expr : plan->expressions) {
			parent_propagate_null_values &= expr->PropagatesNullValues();
		}
		plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
		// then we replace any correlated expressions with the corresponding entry in the correlated_map
		RewriteCorrelatedExpressions rewriter(base_binding, correlated_map);
		rewriter.VisitOperator(*plan);
		// now we add all the columns of the delim_scan to the grouping operators AND the projection list
		idx_t delim_table_index;
		idx_t delim_column_offset;
		idx_t delim_data_offset;
		auto new_group_count = perform_delim ? correlated_columns.size() : 1;
		for (idx_t i = 0; i < new_group_count; i++) {
			auto &col = correlated_columns[i];
			auto colref = make_unique<BoundColumnRefExpression>(
			    col.name, col.type, ColumnBinding(base_binding.table_index, base_binding.column_index + i));
			for (auto &set : aggr.grouping_sets) {
				set.insert(aggr.groups.size());
			}
			aggr.groups.push_back(move(colref));
		}
		if (!perform_delim) {
			// if we are not performing the duplicate elimination, we have only added the row_id column to the grouping
			// operators in this case, we push a FIRST aggregate for each of the remaining expressions
			delim_table_index = aggr.aggregate_index;
			delim_column_offset = aggr.expressions.size();
			delim_data_offset = aggr.groups.size();
			for (idx_t i = 0; i < correlated_columns.size(); i++) {
				auto &col = correlated_columns[i];
				auto first_aggregate = FirstFun::GetFunction(col.type);
				auto colref = make_unique<BoundColumnRefExpression>(
				    col.name, col.type, ColumnBinding(base_binding.table_index, base_binding.column_index + i));
				vector<unique_ptr<Expression>> aggr_children;
				aggr_children.push_back(move(colref));
				auto first_fun = make_unique<BoundAggregateExpression>(move(first_aggregate), move(aggr_children),
				                                                       nullptr, nullptr, false);
				aggr.expressions.push_back(move(first_fun));
			}
		} else {
			delim_table_index = aggr.group_index;
			delim_column_offset = aggr.groups.size() - correlated_columns.size();
			delim_data_offset = aggr.groups.size();
		}
		if (aggr.groups.size() == new_group_count) {
			// we have to perform a LEFT OUTER JOIN between the result of this aggregate and the delim scan
			// FIXME: this does not always have to be a LEFT OUTER JOIN, depending on whether aggr.expressions return
			// NULL or a value
			unique_ptr<LogicalComparisonJoin> join = make_unique<LogicalComparisonJoin>(JoinType::INNER);
			for (auto &aggr_exp : aggr.expressions) {
				auto b_aggr_exp = (BoundAggregateExpression *)aggr_exp.get();
				if (!b_aggr_exp->PropagatesNullValues() || any_join || !parent_propagate_null_values) {
					join = make_unique<LogicalComparisonJoin>(JoinType::LEFT);
					break;
				}
			}
			auto left_index = binder.GenerateTableIndex();
			auto delim_scan = make_unique<LogicalDelimGet>(left_index, delim_types);
			join->children.push_back(move(delim_scan));
			join->children.push_back(move(plan));
			for (idx_t i = 0; i < new_group_count; i++) {
				auto &col = correlated_columns[i];
				JoinCondition cond;
				cond.left = make_unique<BoundColumnRefExpression>(col.name, col.type, ColumnBinding(left_index, i));
				cond.right = make_unique<BoundColumnRefExpression>(
				    correlated_columns[i].type, ColumnBinding(delim_table_index, delim_column_offset + i));
				cond.comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
				join->conditions.push_back(move(cond));
			}
			// for any COUNT aggregate we replace references to the column with: CASE WHEN COUNT(*) IS NULL THEN 0
			// ELSE COUNT(*) END
			for (idx_t i = 0; i < aggr.expressions.size(); i++) {
				D_ASSERT(aggr.expressions[i]->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
				auto bound = (BoundAggregateExpression *)&*aggr.expressions[i];
				vector<LogicalType> arguments;
				if (bound->function == CountFun::GetFunction() || bound->function == CountStarFun::GetFunction()) {
					// have to replace this ColumnBinding with the CASE expression
					replacement_map[ColumnBinding(aggr.aggregate_index, i)] = i;
				}
			}
			// now we update the delim_index
			base_binding.table_index = left_index;
			this->delim_offset = base_binding.column_index = 0;
			this->data_offset = 0;
			return move(join);
		} else {
			// update the delim_index
			base_binding.table_index = delim_table_index;
			this->delim_offset = base_binding.column_index = delim_column_offset;
			this->data_offset = delim_data_offset;
			return plan;
		}
	}
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
		// cross product
		// push into both sides of the plan
		bool left_has_correlation = has_correlated_expressions.find(plan->children[0].get())->second;
		bool right_has_correlation = has_correlated_expressions.find(plan->children[1].get())->second;
		if (!right_has_correlation) {
			// only left has correlation: push into left
			plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
			return plan;
		}
		if (!left_has_correlation) {
			// only right has correlation: push into right
			plan->children[1] = PushDownDependentJoinInternal(move(plan->children[1]), parent_propagate_null_values);
			return plan;
		}
		// both sides have correlation
		// turn into an inner join
		auto join = make_unique<LogicalComparisonJoin>(JoinType::INNER);
		plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
		auto left_binding = this->base_binding;
		plan->children[1] = PushDownDependentJoinInternal(move(plan->children[1]), parent_propagate_null_values);
		// add the correlated columns to the join conditions
		for (idx_t i = 0; i < correlated_columns.size(); i++) {
			JoinCondition cond;
			cond.left = make_unique<BoundColumnRefExpression>(
			    correlated_columns[i].type, ColumnBinding(left_binding.table_index, left_binding.column_index + i));
			cond.right = make_unique<BoundColumnRefExpression>(
			    correlated_columns[i].type, ColumnBinding(base_binding.table_index, base_binding.column_index + i));
			cond.comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
			join->conditions.push_back(move(cond));
		}
		join->children.push_back(move(plan->children[0]));
		join->children.push_back(move(plan->children[1]));
		return move(join);
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = (LogicalJoin &)*plan;
		D_ASSERT(plan->children.size() == 2);
		// check the correlated expressions in the children of the join
		bool left_has_correlation = has_correlated_expressions.find(plan->children[0].get())->second;
		bool right_has_correlation = has_correlated_expressions.find(plan->children[1].get())->second;

		if (join.join_type == JoinType::INNER) {
			// inner join
			if (!right_has_correlation) {
				// only left has correlation: push into left
				plan->children[0] =
				    PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
				return plan;
			}
			if (!left_has_correlation) {
				// only right has correlation: push into right
				plan->children[1] =
				    PushDownDependentJoinInternal(move(plan->children[1]), parent_propagate_null_values);
				return plan;
			}
		} else if (join.join_type == JoinType::LEFT) {
			// left outer join
			if (!right_has_correlation) {
				// only left has correlation: push into left
				plan->children[0] =
				    PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
				return plan;
			}
		} else if (join.join_type == JoinType::RIGHT) {
			// left outer join
			if (!left_has_correlation) {
				// only right has correlation: push into right
				plan->children[1] =
				    PushDownDependentJoinInternal(move(plan->children[1]), parent_propagate_null_values);
				return plan;
			}
		} else if (join.join_type == JoinType::MARK) {
			if (right_has_correlation) {
				throw Exception("MARK join with correlation in RHS not supported");
			}
			// push the child into the LHS
			plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
			// rewrite expressions in the join conditions
			RewriteCorrelatedExpressions rewriter(base_binding, correlated_map);
			rewriter.VisitOperator(*plan);
			return plan;
		} else {
			throw Exception("Unsupported join type for flattening correlated subquery");
		}
		// both sides have correlation
		// push into both sides
		plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
		auto left_binding = this->base_binding;
		plan->children[1] = PushDownDependentJoinInternal(move(plan->children[1]), parent_propagate_null_values);
		auto right_binding = this->base_binding;
		// NOTE: for OUTER JOINS it matters what the BASE BINDING is after the join
		// for the LEFT OUTER JOIN, we want the LEFT side to be the base binding after we push
		// because the RIGHT binding might contain NULL values
		if (join.join_type == JoinType::LEFT) {
			this->base_binding = left_binding;
		} else if (join.join_type == JoinType::RIGHT) {
			this->base_binding = right_binding;
		}
		// add the correlated columns to the join conditions
		for (idx_t i = 0; i < correlated_columns.size(); i++) {
			auto left = make_unique<BoundColumnRefExpression>(
			    correlated_columns[i].type, ColumnBinding(left_binding.table_index, left_binding.column_index + i));
			auto right = make_unique<BoundColumnRefExpression>(
			    correlated_columns[i].type, ColumnBinding(right_binding.table_index, right_binding.column_index + i));

			if (join.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
				JoinCondition cond;
				cond.left = move(left);
				cond.right = move(right);
				cond.comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;

				auto &comparison_join = (LogicalComparisonJoin &)join;
				comparison_join.conditions.push_back(move(cond));
			} else {
				auto &any_join = (LogicalAnyJoin &)join;
				auto comparison = make_unique<BoundComparisonExpression>(ExpressionType::COMPARE_NOT_DISTINCT_FROM,
				                                                         move(left), move(right));
				auto conjunction = make_unique<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
				                                                           move(comparison), move(any_join.condition));
				any_join.condition = move(conjunction);
			}
		}
		// then we replace any correlated expressions with the corresponding entry in the correlated_map
		RewriteCorrelatedExpressions rewriter(right_binding, correlated_map);
		rewriter.VisitOperator(*plan);
		return plan;
	}
	case LogicalOperatorType::LOGICAL_LIMIT: {
		auto &limit = (LogicalLimit &)*plan;
		if (limit.limit || limit.offset) {
			throw ParserException("Non-constant limit or offset not supported in correlated subquery");
		}
		auto rownum_alias = "limit_rownum";
		unique_ptr<LogicalOperator> child;
		unique_ptr<LogicalOrder> order_by;

		// check if the direct child of this LIMIT node is an ORDER BY node, if so, keep it separate
		// this is done for an optimization to avoid having to compute the total order
		if (plan->children[0]->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
			order_by = unique_ptr_cast<LogicalOperator, LogicalOrder>(move(plan->children[0]));
			child = PushDownDependentJoinInternal(move(order_by->children[0]), parent_propagate_null_values);
		} else {
			child = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
		}
		auto child_column_count = child->GetColumnBindings().size();
		// we push a row_number() OVER (PARTITION BY [correlated columns])
		auto window_index = binder.GenerateTableIndex();
		auto window = make_unique<LogicalWindow>(window_index);
		auto row_number = make_unique<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT,
		                                                     nullptr, nullptr);
		auto partition_count = perform_delim ? correlated_columns.size() : 1;
		for (idx_t i = 0; i < partition_count; i++) {
			auto &col = correlated_columns[i];
			auto colref = make_unique<BoundColumnRefExpression>(
			    col.name, col.type, ColumnBinding(base_binding.table_index, base_binding.column_index + i));
			row_number->partitions.push_back(move(colref));
		}
		if (order_by) {
			// optimization: if there is an ORDER BY node followed by a LIMIT
			// rather than computing the entire order, we push the ORDER BY expressions into the row_num computation
			// this way, the order only needs to be computed per partition
			row_number->orders = move(order_by->orders);
		}
		row_number->start = WindowBoundary::UNBOUNDED_PRECEDING;
		row_number->end = WindowBoundary::CURRENT_ROW_ROWS;
		window->expressions.push_back(move(row_number));
		window->children.push_back(move(child));

		// add a filter based on the row_number
		// the filter we add is "row_number > offset AND row_number <= offset + limit"
		auto filter = make_unique<LogicalFilter>();
		unique_ptr<Expression> condition;
		auto row_num_ref =
		    make_unique<BoundColumnRefExpression>(rownum_alias, LogicalType::BIGINT, ColumnBinding(window_index, 0));

		int64_t upper_bound_limit = NumericLimits<int64_t>::Maximum();
		TryAddOperator::Operation(limit.offset_val, limit.limit_val, upper_bound_limit);
		auto upper_bound = make_unique<BoundConstantExpression>(Value::BIGINT(upper_bound_limit));
		condition = make_unique<BoundComparisonExpression>(ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                                                   row_num_ref->Copy(), move(upper_bound));
		// we only need to add "row_number >= offset + 1" if offset is bigger than 0
		if (limit.offset_val > 0) {
			auto lower_bound = make_unique<BoundConstantExpression>(Value::BIGINT(limit.offset_val));
			auto lower_comp = make_unique<BoundComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN,
			                                                         row_num_ref->Copy(), move(lower_bound));
			auto conj = make_unique<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, move(lower_comp),
			                                                    move(condition));
			condition = move(conj);
		}
		filter->expressions.push_back(move(condition));
		filter->children.push_back(move(window));
		// we prune away the row_number after the filter clause using the projection map
		for (idx_t i = 0; i < child_column_count; i++) {
			filter->projection_map.push_back(i);
		}
		return move(filter);
	}
	case LogicalOperatorType::LOGICAL_LIMIT_PERCENT: {
		// NOTE: limit percent could be supported in a manner similar to the LIMIT above
		// but instead of filtering by an exact number of rows, the limit should be expressed as
		// COUNT computed over the partition multiplied by the percentage
		throw ParserException("Limit percent operator not supported in correlated subquery");
	}
	case LogicalOperatorType::LOGICAL_WINDOW: {
		auto &window = (LogicalWindow &)*plan;
		// push into children
		plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
		// add the correlated columns to the PARTITION BY clauses in the Window
		for (auto &expr : window.expressions) {
			D_ASSERT(expr->GetExpressionClass() == ExpressionClass::BOUND_WINDOW);
			auto &w = (BoundWindowExpression &)*expr;
			for (idx_t i = 0; i < correlated_columns.size(); i++) {
				w.partitions.push_back(make_unique<BoundColumnRefExpression>(
				    correlated_columns[i].type,
				    ColumnBinding(base_binding.table_index, base_binding.column_index + i)));
			}
		}
		return plan;
	}
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_UNION: {
		auto &setop = (LogicalSetOperation &)*plan;
		// set operator, push into both children
		plan->children[0] = PushDownDependentJoin(move(plan->children[0]));
		plan->children[1] = PushDownDependentJoin(move(plan->children[1]));
		// we have to refer to the setop index now
		base_binding.table_index = setop.table_index;
		base_binding.column_index = setop.column_count;
		setop.column_count += correlated_columns.size();
		return plan;
	}
	case LogicalOperatorType::LOGICAL_DISTINCT:
		plan->children[0] = PushDownDependentJoin(move(plan->children[0]));
		return plan;
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET: {
		// expression get
		// first we flatten the dependent join in the child
		plan->children[0] = PushDownDependentJoinInternal(move(plan->children[0]), parent_propagate_null_values);
		// then we replace any correlated expressions with the corresponding entry in the correlated_map
		RewriteCorrelatedExpressions rewriter(base_binding, correlated_map);
		rewriter.VisitOperator(*plan);
		// now we add all the correlated columns to each of the expressions of the expression scan
		auto expr_get = (LogicalExpressionGet *)plan.get();
		for (idx_t i = 0; i < correlated_columns.size(); i++) {
			for (auto &expr_list : expr_get->expressions) {
				auto colref = make_unique<BoundColumnRefExpression>(
				    correlated_columns[i].type, ColumnBinding(base_binding.table_index, base_binding.column_index + i));
				expr_list.push_back(move(colref));
			}
			expr_get->expr_types.push_back(correlated_columns[i].type);
		}

		base_binding.table_index = expr_get->table_index;
		this->delim_offset = base_binding.column_index = expr_get->expr_types.size() - correlated_columns.size();
		this->data_offset = 0;
		return plan;
	}
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		plan->children[0] = PushDownDependentJoin(move(plan->children[0]));
		return plan;
	default:
		throw InternalException("Logical operator type \"%s\" for dependent join", LogicalOperatorToString(plan->type));
	}
}

} // namespace duckdb
