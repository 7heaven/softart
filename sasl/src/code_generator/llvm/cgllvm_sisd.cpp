#include <sasl/include/code_generator/llvm/cgllvm_sisd.h>

#include <sasl/include/code_generator/llvm/cgllvm_intrins.h>
#include <sasl/include/code_generator/llvm/cgllvm_impl.imp.h>
#include <sasl/include/code_generator/llvm/cgllvm_globalctxt.h>
#include <sasl/include/code_generator/llvm/cgllvm_caster.h>
#include <sasl/include/code_generator/llvm/cgs.h>

#include <sasl/include/semantic/name_mangler.h>
#include <sasl/include/semantic/semantic_infos.h>
#include <sasl/include/semantic/semantic_infos.imp.h>
#include <sasl/include/semantic/semantics.h>
#include <sasl/include/semantic/symbol.h>
#include <sasl/include/semantic/caster.h>
#include <sasl/include/syntax_tree/declaration.h>
#include <sasl/include/syntax_tree/expression.h>
#include <sasl/include/syntax_tree/program.h>
#include <sasl/include/syntax_tree/statement.h>

#include <sasl/enums/enums_utility.h>

#include <eflib/include/diagnostics/assert.h>

#include <eflib/include/platform/disable_warnings.h>
#include <llvm/BasicBlock.h>
#include <llvm/Constants.h>
#include <llvm/Function.h>
#include <llvm/Module.h>
#include <llvm/Support/IRBuilder.h>
#include <eflib/include/platform/enable_warnings.h>

#include <eflib/include/platform/boost_begin.h>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/assign/std/vector.hpp>
#include <boost/scope_exit.hpp>
#include <boost/bind.hpp>
#include <eflib/include/platform/boost_end.h>

using namespace llvm;
using namespace sasl::syntax_tree;
using namespace boost::assign;
using namespace sasl::utility;

using sasl::semantic::extract_semantic_info;
using sasl::semantic::symbol;
using sasl::semantic::type_info_si;
using sasl::semantic::storage_si;
using sasl::semantic::const_value_si;
using sasl::semantic::call_si;
using sasl::semantic::fnvar_si;
using sasl::semantic::operator_name;
using sasl::semantic::tid_t;
using sasl::semantic::statement_si;
using sasl::semantic::caster_t;

using boost::addressof;
using boost::any_cast;
using boost::bind;
using boost::weak_ptr;

using std::vector;
using std::string;
using std::pair;
using std::make_pair;

#define SASL_VISITOR_TYPE_NAME cgllvm_sisd

BEGIN_NS_SASL_CODE_GENERATOR();

cgllvm_sisd::~cgllvm_sisd(){
}

value_t cgllvm_sisd::emit_logic_op( any const& ctxt_init, operators op, shared_ptr<node> const& left, shared_ptr<node> const& right )
{
	any child_ctxt;
	visit_child( child_ctxt, ctxt_init, left );
	visit_child( child_ctxt, ctxt_init, right );

	if( op == operators::logic_or )
	{
		return service()->emit_or( node_ctxt(left)->value(), node_ctxt(right)->value() );
	}
	else
	{
		return service()->emit_and( node_ctxt(left)->value(), node_ctxt(right)->value() );
	}
}

value_t cgllvm_sisd::emit_short_cond( any const& ctxt_init, shared_ptr<node> const& cond, shared_ptr<node> const& yes, shared_ptr<node> const& no )
{
	// NOTE
	//  If 'yes' and 'no' expression are all reference/variable,
	//  and left is as same abi as right, it will return a reference,
	//  otherwise we will return a value.
	any child_ctxt;

	visit_child( child_ctxt, ctxt_init, cond );
	value_t cond_value = node_ctxt( cond, false)->value().to_rvalue();
	insert_point_t cond_ip = insert_point();

	insert_point_t yes_ip_beg = new_block( "yes_expr", true );
	if( cond != yes ){
		visit_child( child_ctxt, ctxt_init, yes );
	}
	value_t yes_value = node_ctxt( yes, false )->value();
	Value* yes_v = yes_value.load();
	Value* yes_ref = yes_value.load_ref();
	insert_point_t yes_ip_end = insert_point();

	insert_point_t no_ip_beg = new_block( "no_expr", true );
	if( cond != no ){
		visit_child( child_ctxt, ctxt_init, no );
	}
	value_t no_value = node_ctxt( no, false )->value();
	Value* no_ref = ( no_value.abi() == yes_value.abi() ) ? no_value.load_ref() : NULL;
	Value* no_v = no_value.load( yes_value.abi() );
	insert_point_t no_ip_end = insert_point();

	set_insert_point(cond_ip);
	jump_cond( cond_value, yes_ip_beg, no_ip_beg );

	insert_point_t merge_ip = new_block( "cond_merge", false );
	set_insert_point( yes_ip_end );
	jump_to( merge_ip );
	set_insert_point( no_ip_end );
	jump_to( merge_ip );

	set_insert_point(merge_ip);
	value_t result_value;
	Value*		merged = phi_( yes_ip_end.block, yes_v, no_ip_end.block, no_v );
	value_kinds	vkind = (yes_ref && no_ref) ? vkind_ref : vkind_value;
	result_value = create_value( yes_value.tyinfo(), yes_value.hint(), merged, vkind, yes_value.abi() );

	return result_value;
}

SASL_VISIT_DEF( member_expression ){

	any child_ctxt = *data;
	sc_ptr(child_ctxt)->clear_data();
	visit_child( child_ctxt, v.expr );

	cgllvm_sctxt* agg_ctxt = node_ctxt( v.expr );
	assert( agg_ctxt );

	// Aggregated value
	type_info_si* tisi = dynamic_cast<type_info_si*>( v.expr->semantic_info().get() );

	if( tisi->type_info()->is_builtin() ){
		// Swizzle or write mask
		uint32_t masks = v.si_ptr<storage_si>()->swizzle();
		value_t agg_value = agg_ctxt->value();
		if( is_scalar(tisi->type_info()->tycode) ){
			agg_value = service()->cast_s2v(agg_value);
		}
		sc_ptr(data)->value() = emit_extract_elem_mask( agg_value, masks );
	} else {
		// Member
		shared_ptr<symbol> struct_sym = tisi->type_info()->symbol();
		shared_ptr<symbol> mem_sym = struct_sym->find_this( v.member->str );

		assert( mem_sym );
		cgllvm_sctxt* mem_ctxt = node_ctxt( mem_sym->node(), true );
		sc_ptr(data)->value() = mem_ctxt->value();
		sc_ptr(data)->value().parent( agg_ctxt->value() );
		sc_ptr(data)->value().abi( agg_ctxt->value().abi() );
	}

	node_ctxt(v, true)->copy( sc_ptr(data) );
}

SASL_VISIT_DEF( cond_expression ){
	any child_ctxt_init = *data;
	sc_ptr( &child_ctxt_init )->clear_data();
	
	sc_ptr(data)->value()
		= emit_short_cond( child_ctxt_init, v.cond_expr, v.yes_expr, v.no_expr ) ;
	node_ctxt( v, true )->copy( sc_ptr(data) );
}

SASL_VISIT_DEF( unary_expression ){
	any child_ctxt_init = *data;

	any child_ctxt;
	visit_child( child_ctxt, child_ctxt_init, v.expr );
	
	value_t inner_value = sc_ptr(&child_ctxt)->value();

	shared_ptr<value_tyinfo> one_tyinfo = create_tyinfo( v.si_ptr<type_info_si>()->type_info() );
	builtin_types hint = inner_value.hint();

	if( v.op == operators::negative ){
		value_t zero_value = null_value( one_tyinfo->hint(), inner_value.abi() );
		sc_ptr(data)->value() = emit_sub(zero_value, inner_value);
	} else if( v.op == operators::positive ){
		sc_ptr(data)->value() = inner_value;
	} else if( v.op == operators::logic_not ) {
		value_t mask_value = create_constant_int( NULL, hint, inner_value.abi(), 1 );
		sc_ptr(data)->value() = emit_bit_xor( mask_value, inner_value );
	} else if( v.op == operators::bit_not ) {
		value_t all_one_value = create_constant_int( NULL, hint, inner_value.abi(), 0xFFFFFFFFFFFFFFFF );
		sc_ptr(data)->value() = emit_bit_xor( all_one_value, inner_value );
	} else {

		value_t one_value = service()->create_constant_int( one_tyinfo.get(), builtin_types::none, inner_value.abi(), 1 ) ;

		if( v.op == operators::prefix_incr ){
			value_t inc_v = emit_add( inner_value, one_value );
			inner_value.store( inc_v );
			sc_ptr(data)->value() = inner_value;
		} else if( v.op == operators::prefix_decr ){
			value_t dec_v = emit_sub( inner_value, one_value );
			inner_value.store( dec_v );
			sc_ptr(data)->value() = inner_value;
		} else if( v.op == operators::postfix_incr ){
			sc_ptr(data)->value() = inner_value.to_rvalue();
			inner_value.store( emit_add( inner_value, one_value ) );
		} else if( v.op == operators::postfix_decr ){
			sc_ptr(data)->value() = inner_value.to_rvalue();
			inner_value.store( emit_sub( inner_value, one_value ) );
		}
	}
	
	node_ctxt(v, true)->copy( sc_ptr(data) );
	node_ctxt(v, true)->data().tyinfo = one_tyinfo;
}

SASL_VISIT_DEF_UNIMPL( statement );

SASL_VISIT_DEF( compound_statement ){
	any child_ctxt_init = *data;
	any child_ctxt;

	sc_env_ptr(&child_ctxt_init)->sym = v.symbol();

	for ( std::vector< boost::shared_ptr<statement> >::iterator it = v.stmts.begin();
		it != v.stmts.end(); ++it)
	{
		visit_child( child_ctxt, child_ctxt_init, *it );
	}

	node_ctxt(v, true)->copy( sc_ptr(data) );
}

SASL_VISIT_DEF( if_statement ){
	any child_ctxt_init = *data;
	any child_ctxt;

	if_cond_beg();
	visit_child( child_ctxt, child_ctxt_init, v.cond );
	tid_t cond_tid = extract_semantic_info<type_info_si>(v.cond)->entry_id();
	tid_t bool_tid = msi->pety()->get( builtin_types::_boolean );
	if( cond_tid != bool_tid ){
		if( caster->cast(msi->pety()->get_proto(bool_tid), v.cond.get()) == caster_t::nocast ){
			assert(false);
		}
	}
	value_t cond_value = node_ctxt( v.cond, false )->value();
	if_cond_end( cond_value );

	insert_point_t ip_cond = insert_point();

	insert_point_t ip_yes_beg = new_block( "if.yes", true );
	then_beg();
	visit_child( child_ctxt, child_ctxt_init, v.yes_stmt );
	then_end();
	insert_point_t ip_yes_end = insert_point();

	insert_point_t ip_no_beg, ip_no_end;
	if( v.no_stmt ){
		ip_no_beg = new_block( "if.no", true );
		else_beg();
		visit_child( child_ctxt, child_ctxt_init, v.no_stmt );
		else_end();
		ip_no_end = insert_point();
	}

	insert_point_t ip_merge = new_block( "if.end", false );

	// Fill back.
	set_insert_point( ip_cond );
	jump_cond( cond_value, ip_yes_beg, ip_no_beg ? ip_no_beg : ip_merge );

	set_insert_point( ip_yes_end );
	jump_to( ip_merge );

	if( ip_no_end ){
		set_insert_point( ip_no_end );
		jump_to(ip_merge);
	}

	set_insert_point( ip_merge );

	node_ctxt(v, true)->copy( sc_ptr(data) );
}

SASL_VISIT_DEF( while_statement ){
	any child_ctxt_init = *data;
	any child_ctxt;

	insert_point_t cond_beg = new_block( "while.cond", true );
	visit_child( child_ctxt, child_ctxt_init, v.cond );
	tid_t cond_tid = extract_semantic_info<type_info_si>(v.cond)->entry_id();
	tid_t bool_tid = msi->pety()->get( builtin_types::_boolean );
	if( cond_tid != bool_tid ){
		caster->cast( msi->pety()->get_proto(bool_tid), v.cond.get() );
	}
	insert_point_t cond_end = insert_point();

	insert_point_t break_beg = new_block( "while.break", true );
	insert_point_t break_end = insert_point();

	insert_point_t body_beg = new_block( "while.body", true );
	sc_env_ptr( &child_ctxt_init )->continue_to = cond_beg;
	sc_env_ptr( &child_ctxt_init )->break_to = break_beg;
	visit_child( child_ctxt, child_ctxt_init, v.body );
	insert_point_t body_end = insert_point();
	
	insert_point_t while_end = new_block( "while.end", true );
	
	// Fill back
	set_insert_point( cond_end );
	jump_cond( node_ctxt( v.cond )->value(), body_beg, break_beg );

	set_insert_point( break_end );
	jump_to( while_end );

	set_insert_point( body_end );
	jump_to( cond_beg );

	set_insert_point( while_end );
}

SASL_VISIT_DEF( dowhile_statement ){
	any child_ctxt_init = *data;
	any child_ctxt;

	insert_point_t do_beg = new_block( "do.to_body", true );
	insert_point_t do_end = insert_point();

	insert_point_t cond_beg = new_block( "while.cond", true );
	visit_child( child_ctxt, child_ctxt_init, v.cond );
	tid_t cond_tid = extract_semantic_info<type_info_si>(v.cond)->entry_id();
	tid_t bool_tid = msi->pety()->get( builtin_types::_boolean );
	if( cond_tid != bool_tid ){
		if ( caster->cast( msi->pety()->get_proto(bool_tid), v.cond.get() ) == caster_t::nocast ){
			assert( false );
		}
	}
	insert_point_t cond_end = insert_point();

	insert_point_t break_beg = new_block( "while.break", true );
	insert_point_t break_end = insert_point();

	insert_point_t body_beg = new_block( "while.body", true );
	sc_env_ptr( &child_ctxt_init )->continue_to = cond_beg;
	sc_env_ptr( &child_ctxt_init )->break_to = break_beg;
	visit_child( child_ctxt, child_ctxt_init, v.body );
	insert_point_t body_end = insert_point();
	
	insert_point_t while_end = new_block( "while.end", true );
	
	// Fill back
	set_insert_point( do_end );
	jump_to( body_beg );

	set_insert_point( cond_end );
	jump_cond( node_ctxt( v.cond )->value(), body_beg, break_beg );

	set_insert_point( break_end );
	jump_to( while_end );

	set_insert_point( body_end );
	jump_to( cond_beg );

	set_insert_point( while_end );
}

SASL_VISIT_DEF( case_label ){
	any child_ctxt_init = *data;
	any child_ctxt;

	if( v.expr ){
		visit_child( child_ctxt, child_ctxt_init, v.expr );
	}
}

SASL_VISIT_DEF_UNIMPL( ident_label );

SASL_VISIT_DEF( switch_statement ){
	any child_ctxt_init = *data;
	any child_ctxt;

	visit_child( child_ctxt, child_ctxt_init, v.cond );
	insert_point_t cond_end = insert_point();

	insert_point_t break_end = new_block( "switch.break", true );

	insert_point_t body_beg = new_block( "switch.body", true );
	sc_env_ptr(&child_ctxt_init)->break_to = break_end;
	visit_child( child_ctxt, child_ctxt_init, v.stmts );
	insert_point_t body_end = insert_point();

	insert_point_t switch_end = new_block( "switch.end", true );

	// Collect Labeled Statement Position
	vector< pair<value_t,insert_point_t> > cases;
	statement_si* ssi = v.si_ptr<statement_si>();
	assert( ssi );

	insert_point_t default_beg = switch_end;
	BOOST_FOREACH( weak_ptr<labeled_statement> const& weak_lbl_stmt, ssi->labels() ){
		shared_ptr<labeled_statement> lbl_stmt = weak_lbl_stmt.lock();
		assert( lbl_stmt );

		insert_point_t stmt_ip = node_ctxt(lbl_stmt)->data().position; 
		BOOST_FOREACH( shared_ptr<label> const& lbl, lbl_stmt->labels ){
			assert( lbl->node_class() == node_ids::case_label );
			shared_ptr<case_label> case_lbl = lbl->as_handle<case_label>();
			if( case_lbl->expr ){
				value_t v = node_ctxt( case_lbl->expr )->value();
				cases.push_back( make_pair(v, stmt_ip ) );
			} else {
				default_beg = stmt_ip;
			}
		}
	}
	
	// Fill back jumps
	set_insert_point(cond_end);
	value_t cond_v = node_ctxt( v.cond )->value();
	switch_to( cond_v, cases, default_beg );

	set_insert_point( break_end );
	jump_to( switch_end );

	set_insert_point( body_end );
	jump_to( switch_end );

	set_insert_point( switch_end );
}

SASL_VISIT_DEF( labeled_statement ){
	any child_ctxt_init = *data;
	any child_ctxt;

	BOOST_FOREACH( shared_ptr<label> const& lbl, v.labels ){
		// Constant expression, no instruction was generated.
		visit_child( child_ctxt, child_ctxt_init, lbl );
	}
	insert_point_t stmt_pos = new_block( "switch.case", true );
	visit_child( child_ctxt, child_ctxt_init, v.stmt );
	node_ctxt(v, true)->data().position = stmt_pos;

	sc_ptr(data)->copy( node_ctxt(v) );
}

SASL_VISIT_DEF( for_statement ){
	any child_ctxt_init = *data;
	any child_ctxt;

	sc_env_ptr(&child_ctxt_init)->sym = v.symbol();
	// For instructions layout:
	//		... before code ...
	//		for initializer
	//		goto for_cond
	//	for_cond:
	//		condition expression
	//		goto for_body
	//	for_iter:
	//		iteration code
	//		goto for_cond
	//	for_break:
	//		goto the for_end
	//	for_body:
	//		...
	//		...
	//		goto for_cond
	// This design is to avoid fill-back that need to store lots of break and continue points.

	visit_child( child_ctxt, child_ctxt_init, v.init );
	insert_point_t init_end = insert_point();

	insert_point_t cond_beg = new_block( "for_cond", true );
	if( v.cond ){ visit_child( child_ctxt, child_ctxt_init, v.cond ); }
	insert_point_t cond_end = insert_point();

	insert_point_t iter_beg = new_block( "for_iter", true );
	if( v.iter ){ visit_child( child_ctxt, child_ctxt_init, v.iter ); }
	insert_point_t iter_end = insert_point();

	insert_point_t for_break = new_block( "for_break", true );

	insert_point_t body_beg = new_block( "for_body", true );
	sc_env_ptr(&child_ctxt_init)->continue_to = iter_beg;
	sc_env_ptr(&child_ctxt_init)->break_to = for_break;
	visit_child( child_ctxt, child_ctxt_init, v.body );
	insert_point_t body_end = insert_point();

	insert_point_t for_end = new_block( "for_end", true );

	// Fill back jumps
	set_insert_point( init_end );
	jump_to( cond_beg );

	set_insert_point( cond_end );
	if( !v.cond ){
		jump_to( body_beg );
	} else {
		jump_cond( node_ctxt( v.cond, false )->value(), body_beg, for_end );
	}

	set_insert_point( iter_end );
	jump_to( cond_beg );

	set_insert_point( body_end );
	jump_to( iter_beg );

	set_insert_point( for_break );
	jump_to( for_end );

	// Set correct out block.
	set_insert_point( for_end );

	node_ctxt(v, true)->copy( sc_ptr(data) );
}

SASL_SPECIFIC_VISIT_DEF( visit_continue	, jump_statement )
{
	assert( sc_env_ptr(data)->continue_to );
	jump_to( sc_env_ptr(data)->continue_to );
}

SASL_SPECIFIC_VISIT_DEF( visit_break	, jump_statement )
{
	assert( sc_env_ptr(data)->break_to );
	jump_to( sc_env_ptr(data)->break_to );
}

cgllvm_sctxt* cgllvm_sisd::node_ctxt( sasl::syntax_tree::node& v, bool create_if_need /*= false */ )
{
	return cgllvm_impl::node_ctxt( &v, create_if_need);
}

llvm_module_impl* cgllvm_sisd::mod_ptr(){
	assert( dynamic_cast<llvm_module_impl*>( mod.get() ) );
	return static_cast<llvm_module_impl*>( mod.get() );
}

cg_service* cgllvm_sisd::service() const
{
	return const_cast<cgllvm_sisd*>(this);
}

abis cgllvm_sisd::local_abi( bool is_c_compatible ) const
{
	return is_c_compatible ? abi_c : abi_llvm;
}

END_NS_SASL_CODE_GENERATOR();