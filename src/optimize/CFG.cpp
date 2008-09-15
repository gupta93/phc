/*
 * phc -- the open source PHP compiler
 * See doc/license/README.license for licensing information
 *
 * Control-flow Graph
 */

#include <boost/foreach.hpp>
#include <boost/graph/graphviz.hpp>
#include <boost/graph/depth_first_search.hpp>
#include <boost/graph/visitors.hpp>
#include <boost/graph/dominator_tree.hpp>
#include <boost/graph/filtered_graph.hpp>
#include <boost/graph/topological_sort.hpp>

#include "CFG.h"
#include "SSA.h"
#include "Def_use.h"
#include "process_ast/DOT_unparser.h"
#include "process_ir/General.h"

using namespace boost;
using namespace std;
using namespace MIR;

CFG::CFG (Method* method)
: dominance (NULL)
, duw (NULL)
, bs()
, method (method)
{
	vb = get(vertex_bb_t(), bs);
	ee = get(edge_cfg_edge_t(), bs);
	index = get(vertex_index_t(), bs);

	// Initialize the entry and exit blocks
	entry = add_bb (new Entry_block (this, method));
	exit = add_bb (new Exit_block (this, method));

	add_statements (method->statements);
}

vertex_t
CFG::add_bb (Basic_block* bb)
{
	vertex_t v = add_vertex (bs);
	vb[v] = bb;
	bb->vertex = v;

	return v;
}

edge_t
CFG::add_edge (Basic_block* source, Basic_block* target)
{
	edge_t e = boost::add_edge (source->vertex, target->vertex, bs).first;
	ee[e] = new Edge (source, target, e);
	return e;
}

pair<edge_t, edge_t>
CFG::add_branch (Branch_block* source, Basic_block* target1, Basic_block* target2)
{
	assert (source);
	edge_t et = boost::add_edge (source->vertex, target1->vertex, bs).first;
	edge_t ef = boost::add_edge (source->vertex, target2->vertex, bs).first;

	ee[et] = new Edge (source, target1, et, true);
	ee[ef] = new Edge (source, target2, ef, false);

	ee[et]->edge = et;
	ee[ef]->edge = ef;

	return pair<edge_t, edge_t> (et, ef);
}

void
CFG::add_statements (Statement_list* statements)
{
	// Keep track of labels, for edges between gotos and branches.
	map <string, vertex_t> labels;

	// In the second pass, we'll need the vertices to add edges.
	map <Statement*, vertex_t> nodes;


	// In the first pass, just create nodes for the statements.
	foreach (Statement* s, *statements)
	{
		vertex_t v;
		switch (s->classid())
		{
			case Label::ID:
				v = add_bb (new Empty_block (this));
				labels [*dyc<Label>(s)->label_name->get_value_as_string ()] = v;
				break;

			case Goto::ID:
				v = add_bb (new Empty_block (this));
				break;

			case Branch::ID:
				v = add_bb (new Branch_block (this, dyc<Branch>(s)));
				break;

			default:
				v = add_bb (new Statement_block (this, s));
				break;
		}
		nodes[s] = v;
	}


	// Create the edges
	vertex_t parent = entry;
	bool use_parent = true; // parent is just an int, so not nullable

	foreach (Statement* s, *statements)
	{
		// Be careful with pointers. Its very easy to overwrite vertices
		vertex_t v = nodes[s];
		if (use_parent)
			add_edge (vb[parent], vb[v]);

		switch (s->classid())
		{
			case Goto::ID:
			{
				vertex_t target = 
					labels[*dyc<Goto>(s)->label_name->get_value_as_string ()];
				add_edge (vb[v], vb[target]);

				use_parent = false;
				break;
			}

			case Branch::ID:
			{
				Branch* b = dyc<Branch>(s);
				vertex_t iftrue = labels[*b->iftrue->get_value_as_string ()];
				vertex_t iffalse = labels[*b->iffalse->get_value_as_string ()];
				add_branch (
					dynamic_cast<Branch_block*> (vb[v]),
					vb[iftrue],
					vb[iffalse]);

				use_parent = false;
				break;
			}

			default:
				parent = v;
				use_parent = true;
				break;
		}
	}
	
	assert (use_parent);
	add_edge (vb[parent], vb[exit]);

	tidy_up ();

	consistency_check ();
}

Basic_block*
CFG::get_entry_bb ()
{
	return vb[entry];
}

Basic_block*
CFG::get_exit_bb ()
{
	return vb[exit];
}

BB_list*
CFG::get_all_bbs ()
{
	BB_list* result = new BB_list;

	foreach (vertex_t v, vertices(bs))
	{
		result->push_back (vb[v]);
	}

	return result;
}

// TODO simplify linearizer
template <class Graph>
class Depth_first_list : public default_dfs_visitor
{
public:
	BB_list* result;

	Depth_first_list (BB_list* result)
	: result (result)
	{
	}

	void discover_vertex (vertex_t v, const Graph& g)
	{
		result->push_back (get(vertex_bb_t(), g)[v]);
	}
};


// Dump to graphviz
struct BB_property_functor
{
	property_map<Graph, vertex_bb_t>::type vb;
	property_map<Graph, edge_cfg_edge_t>::type ee;

	BB_property_functor (CFG* cfg)
	{
		vb = cfg->vb;
		ee = cfg->ee;
	}
	void operator()(std::ostream& out, const edge_t& e) const 
	{
		Edge* edge = ee[e];
		if (indeterminate (edge->direction))
			return;

		if (edge->direction)
			out << "[label=T]";
		else
			out << "[label=F]";

		// Head and tail annotatations are done in the vertex, because the
		// headlabel and taillabel attributes dont expand the area they are
		// in, and so are frequently unreadable.
	}
#define LINE_LENGTH 30
	void operator()(std::ostream& out, const vertex_t& v) const 
	{
		out << "[";

		pair<String*, String*> bb_props;
		foreach (bb_props, *vb[v]->get_graphviz_properties ())
			out << *bb_props.first << "=" << *bb_props.second << ",";

		out << "label=\"";

		// IN annotations
		stringstream ss1;
		pair<String*, Set*> props;
		foreach (props, *vb[v]->get_graphviz_head_properties ())
		{
			if (props.second->size ())
			{
				ss1 << *props.first << " = [";
				unsigned int line_count = 1;
				foreach (VARIABLE_NAME* var_name, *props.second)
				{
					ss1 << *var_name->get_ssa_var_name() << ", ";
					if (ss1.str().size() > (LINE_LENGTH * line_count))
					{
						line_count++;
						ss1 << "\\n";
					}
				}
				ss1 << "]\\n";
			}
		}

		// BB source
		stringstream ss2;
		ss2 << *DOT_unparser::escape (vb[v]->get_graphviz_label ());

		// BB properties
		stringstream ss3;
		foreach (props, *vb[v]->get_graphviz_bb_properties ())
		{
			if (props.second->size ())
			{
				ss3 << *props.first << " = [";
				unsigned int line_count = 1;
				foreach (VARIABLE_NAME* var_name, *props.second)
				{
					ss3 << *var_name->get_ssa_var_name () << ", ";
					if (ss3.str().size() > (LINE_LENGTH * line_count))
					{
						line_count++;
						ss3 << "\\n";
					}
				}
				ss3 << "]\\n";
			}
		}

		// OUT annotations
		stringstream ss4;
		foreach (props, *vb[v]->get_graphviz_tail_properties ())
		{
			if (props.second->size ())
			{
				ss4 << *props.first << " = [";
				unsigned int line_count = 1;
				foreach (VARIABLE_NAME* var_name, *props.second)
				{
					ss4 << *var_name->get_ssa_var_name() << ", ";
					if (ss4.str().size() > (LINE_LENGTH * line_count))
					{
						line_count++;
						ss4 << "\\n";
					}
				}
				ss4 << "]\\n";
			}
		}

		// Print out all 4 stringstreams, with line-break;
		out << ss1.str();
		if (ss1.str().size())
			out << "\\n"; // blank line before source
		out << ss2.str();
		if (ss3.str().size() || ss4.str().size())
			out << "\\n\\n"; // blank line after source
		out << ss3.str();
		out << ss4.str();
		if (ss3.str().size() || ss4.str().size())
			out << "\b\b";

		
		out << "\"]";
	}
};

struct Graph_property_functor
{
	String* label;
	String* method_name;
	Graph_property_functor(String* method_name, String* label) 
	: label (label)
	, method_name (method_name)
	{
	}

	void operator()(std::ostream& out) const 
	{
		out << "graph [outputorder=edgesfirst];" << std::endl;
		out << "graph [label=\"" << *method_name << " - " << *label << "\"];" << std::endl;
	}
};

void
CFG::dump_graphviz (String* label)
{
	renumber_vertex_indices ();
	consistency_check ();
	write_graphviz (
		cout, 
		bs, 
		BB_property_functor(this),
		BB_property_functor(this),
		Graph_property_functor(method->signature->method_name->value, label));
}

/* Error checking */
void
CFG::consistency_check ()
{
	// The graph should never reuse vertices.
	foreach (vertex_t v, vertices (bs))
	{
		assert (vb[v]->vertex == v);
	}
}
// Do a depth first search. For each block, add a label, and a goto to the
// next block(s).
class Linearizer : public default_dfs_visitor
{
	CFG* cfg;
	// Since the linearizer is passed by copy, a non-pointer would be
	// deallocated after the depth-first-search. However, we need to keep the
	// exit_label alive.
	map<vertex_t, LABEL_NAME*>* labels;

public:

	List<Statement*>* statements;
	Linearizer(CFG* cfg) : cfg(cfg)
	{
		statements = new List<Statement*>;
		labels = new map<vertex_t, LABEL_NAME*>;
	}

	/* Assign a label for each block. */
	void initialize_vertex (vertex_t v, const Graph& g)
	{
		(*labels) [v] = fresh_label_name ();
	}

	void discover_vertex (vertex_t v, const Graph& g)
	{
		Basic_block* bb = get(vertex_bb_t(), g)[v];

		// Add a label (the exit block label is added at the very end)
		if (not dynamic_cast<Exit_block*> (bb))
			statements->push_back (new Label((*labels)[v]->clone ()));

		// Statement or branch block
		if (Statement_block* sb = dynamic_cast<Statement_block*> (bb))
			statements->push_back (sb->statement);

		else if (Branch_block* br = dynamic_cast<Branch_block*> (bb))
		{
			// While in the CFG, the ifftrue and iffalse fields of a branch are
			// meaningless (by design).
			statements->push_back (br->branch);
			br->branch->iftrue = (*labels)[br->get_true_successor ()->vertex];
			br->branch->iffalse = (*labels)[br->get_false_successor ()->vertex];
		}

		// Add a goto successor
		if (not dynamic_cast<Branch_block*> (bb)
				&& not dynamic_cast<Exit_block*> (bb))
		{
			vertex_t next = bb->get_successor ()->vertex;
			statements->push_back (new Goto ((*labels)[next]->clone ()));
		}
	}

	void add_exit_label ()
	{
		Basic_block* bb = cfg->get_exit_bb ();

		// Add an exit block at the very end, so that it doesnt fall through to
		// anything.
		statements->push_back (new Label((*labels)[bb->vertex]->clone ()));
	}
};

class Label_counter : public Visitor
{
	map<string, int>* counts;
public:
	Label_counter (map<string, int>* c) : counts(c) {}

	void pre_label_name (LABEL_NAME* in)
	{
		(*counts)[*in->value]++;
	}
};

List<Statement*>*
CFG::get_linear_statements ()
{
	Linearizer linearizer (this);
	renumber_vertex_indices ();
	depth_first_search (bs, visitor (linearizer));
	linearizer.add_exit_label ();
	List<Statement*>* results = linearizer.statements;

	/* Remove redundant gotos, which would fall-through to their targets
	 * anyway. */
	List<Statement*>::iterator i = results->begin ();
	List<Statement*>::iterator end = --results->end ();
	while (i != end) // stop one before the end, so that i++ will still read a statement
	{
		Wildcard<LABEL_NAME>* ln = new Wildcard<LABEL_NAME>;
		Statement* s = *i;
		Statement* next = *(++i);
		if (s->match (new Goto (ln))
			&& next->match (new Label (ln->value)))
		{
			i--;

			// Use this form of looping so that the iterator moves to the next
			// iterm before its invalidated.
			results->erase (i++);
		}
	}

	/* Remove labels that are only used once. */
	map<string, int> label_counts;
	(new Label_counter (&label_counts))->visit_statement_list (results);

	i = results->begin ();
	while (i != results->end ())
	{
		Wildcard<LABEL_NAME>* ln = new Wildcard<LABEL_NAME>;
		if ((*i)->match (new Label (ln))
			&& label_counts[*ln->value->value] == 1)
			results->erase (i++);
		else
			i++;
	}
	return results;
}


void
CFG::renumber_vertex_indices ()
{
	int new_index = 0;
	foreach (vertex_t v, vertices (bs))
		index[v] = new_index++;
}


/* SSA from here */

void CFG::convert_to_ssa_form ()
{
	// Calculate dominance frontiers
	dominance = new Dominance (this);
	dominance->calculate_immediate_dominators ();
	dominance->calculate_local_dominance_frontier ();
	dominance->propagate_dominance_frontier_upwards ();

	// Build def-use web (we're not in SSA form, but this will do the job).
	duw = new Def_use_web ();
	duw->run (this);

	// Muchnick gives up at this point. We continue instead in Cooper/Torczon,
	// Section 9.3.3, with some minor changes. Since we dont have a list of
	// global names, we iterate through all blocks, rather than the blocks
	// corresponding to the variable names. 
	
	// For an assignment to X in BB, add a PHI function for variable X in the
	// dominance frontier of BB.
	// TODO Abstract this.
	BB_list* worklist = get_all_bbs_top_down ();
	BB_list::iterator i = worklist->begin ();
	while (i != worklist->end ())
	{
		Basic_block* bb = *i;
		foreach (Basic_block* frontier, *bb->get_dominance_frontier ())
		{
			// Get defs (including phi node LHSs)
			Set* def_list = bb->get_pre_ssa_defs ();
			foreach (Phi* phi, *bb->get_phi_nodes())
				def_list->insert (phi->lhs);

			bool def_added = false;
			foreach (VARIABLE_NAME* var_name, *def_list)
			{
				if (!frontier->has_phi_function (var_name))
				{
					frontier->add_phi_function (var_name);
					def_added = true;
				}
			}

			// This adds a new def, which requires us to iterate.
			if (def_added)
				worklist->push_back (frontier);
		}
		i++;
	}

	SSA_renaming sr(this);
	sr.rename_vars (get_entry_bb ());

	// Check all variables are converted
	class Check_in_SSA : public Visitor
	{
		void pre_variable_name (VARIABLE_NAME* in)
		{
			assert (in->in_ssa);
		}
	};

	foreach (Basic_block* bb, *get_all_bbs ())
	{
		if (Statement_block* sb = dynamic_cast<Statement_block*> (bb))
			sb->statement->visit (new Check_in_SSA ());
	}

	// Build def-use web
	duw = new Def_use_web ();
	duw->run (this);
}

void
CFG::rebuild_ssa_form ()
{
	duw = new Def_use_web ();
	duw->run (this);
}


void
CFG::convert_out_of_ssa_form ()
{
	foreach (Basic_block* bb, *get_all_bbs ())
	{
		foreach (Phi* phi, *bb->get_phi_nodes ())
		{
			BB_list* preds = bb->get_predecessors ();
			foreach (VARIABLE_NAME* var_name, *phi->get_args ())
			{
				Assign_var* copy = new Assign_var (
					phi->lhs->clone (),
					false,
					var_name->clone ());

				Statement_block* new_bb = new Statement_block (this, copy);

				add_bb_between (preds->front (), bb, new_bb);
				// TODO I'm not sure these are in the same order.
				preds->pop_front ();

				// We avoid the critical edge problem because we have only 1
				// statement per block. Removing phi nodes adds a single block
				// along the necessary edge.
			}
		}
		bb->remove_phi_nodes ();
	}

	// TODO: at this point, we could do with a register-allocation style
	// interference graph to reduce the number of temporaries (aka
	// "registers") that we use in the generated code.
}

BB_list*
CFG::get_bb_successors (Basic_block* bb)
{
	BB_list* result = new BB_list;

	foreach (edge_t e, out_edges (bb->vertex, bs))
	{
		result->push_back (vb[target (e, bs)]);
	}

	return result;
}

BB_list*
CFG::get_bb_predecessors (Basic_block* bb)
{
	BB_list* result = new BB_list;

	foreach (edge_t e, in_edges (bb->vertex, bs))
	{
		result->push_back (vb[source (e, bs)]);
	}

	return result;
}

Edge*
CFG::get_edge (Basic_block* bb1, Basic_block* bb2)
{
	foreach (edge_t e, out_edges (bb1->vertex, bs))
		if (target (e, bs) == bb2->vertex)
			return ee[e];

	assert (0);
}

Edge*
CFG::get_entry_edge ()
{
	return get_entry_bb ()->get_successor_edge ();
}

Edge_list*
CFG::get_all_edges ()
{
	Edge_list* result = new Edge_list;

	foreach (edge_t e, edges(bs))
	{
		result->push_back (ee[e]);
	}
	return result;
}

Edge_list*
CFG::get_edge_successors (Basic_block* bb)
{
	Edge_list* result = new Edge_list;

	foreach (edge_t e, out_edges (bb->vertex, bs))
	{
		result->push_back (ee[e]);
	}

	return result;
}

Edge_list*
CFG::get_edge_predecessors (Basic_block* bb)
{
	Edge_list* result = new Edge_list;

	foreach (edge_t e, in_edges (bb->vertex, bs))
	{
		result->push_back (ee[e]);
	}

	return result;
}

/* returns true or false. If edge isnt true or false, asserts. */
bool
CFG::is_true_edge (Edge* edge)
{
	assert (!indeterminate (edge->direction));
	return edge->direction;
}

void
CFG::add_bb_between (Basic_block* source, Basic_block* target, Basic_block* new_bb)
{
	add_bb (new_bb);
	Edge* current_edge = get_edge (source, target);

	edge_t e1 = add_edge (source, new_bb);
	ee[e1]->direction = current_edge->direction;

	add_edge (new_bb, target);
	boost::remove_edge (current_edge->edge, bs);
}


void
CFG::replace_bb (Basic_block* bb, BB_list* replacements)
{
	if (replacements->size() == 1
		&& replacements->front() == bb)
	{
		// Same BB: do nothing
	}
	else if (replacements->size() == 0)
	{
		// Branch blocks don't go through this interface
		Basic_block* succ = bb->get_successor ();
		Edge* succ_edge = bb->get_successor_edge ();

		// TODO: dont know what to do here.
		// The immediate post-dominator should get the phi nodes, according to
		// Cooper/Torczon lecture notes (see DCE comments).
		// TODO Avoid exponential phi by putting in an empty block.


		// Each predecessor needs a node to each successor.
		foreach (Basic_block* pred, *bb->get_predecessors ())
		{
			edge_t e = add_edge (pred, succ);

			// If the edge has a T/F label, it is because the predecessor is a
			// Branch. Just copy the label from the new predecessor.
			ee[e]->direction = get_edge (pred, bb)->direction;

			// TODO When we replace an edge before a Phi node, we need to update that
			// phi node with the edge coming into it.
			foreach (Phi* phi, *succ->get_phi_nodes ())
				phi->replace_edge (succ_edge, ee[e]);
		}

		// If removing a block causes a successor to have fewer incoming edges,
		// then we should remove the phi arguments for this edge from the phi
		// node.
		if (bb->get_predecessors ()->size() == 0)
			foreach (Phi* phi, *succ->get_phi_nodes ())
				phi->remove_arg_for_edge (succ_edge);

		succ->merge_phi_nodes (bb);

		remove_bb (bb);


		// We don't perform this in the middle of the removal operation, as it will
		// make it non-atomic, which could be tricky. Its over now, so even if it
		// recurses, its fine.
		succ->fix_solo_phi_args ();
	}
	else
	{
		// Get the data from the BB so we can remove it.
		BB_list* preds = bb->get_predecessors ();
		Edge_list* pred_edges = bb->get_predecessor_edges ();
		Phi_list* old_phis = bb->get_phi_nodes ();
		Basic_block* succ = bb->get_successor ();

		remove_bb (bb);

		// Front gets all incoming edges added
		Basic_block* front = replacements->front ();
		replacements->pop_front ();

		add_bb (front);
		// Each predecessor needs a node to each successor.
		foreach (Basic_block* pred, *preds)
		{
			Edge* old_edge = pred_edges->front();
			pred_edges->pop_front ();

			edge_t e = add_edge (pred, front);

			// If the edge has a T/F label, it is because the predecessor is a
			// Branch. Just copy the label from the new predecessor.
			ee[e]->direction = old_edge->direction;

			// When we replace an edge before a Phi node, we need to update that
			// phi node with the edge coming into it.
			foreach (Phi* phi, *old_phis)
				phi->replace_edge (old_edge, ee[e]);
		}

		// Copy the phi nodes into front (the edges are already updated)
		front->merge_phi_nodes (bb);


		// Add edge along the chain
		Basic_block* prev = front;
		foreach (Basic_block* new_bb, *replacements)
		{
			assert (!isa<Branch_block> (new_bb));
			add_bb (new_bb);
			add_edge (prev, new_bb);
			prev = new_bb;
		}

		// There is only 1 successor
		add_edge (prev, succ);
	}

}

void
CFG::remove_bb (Basic_block* bb)
{
	clear_vertex (bb->vertex, bs);
	remove_vertex (bb->vertex, bs);
}

void
CFG::remove_edge (Edge* edge)
{
	boost::remove_edge (edge->edge, bs);
	tidy_up ();
}



struct filter_back_edges
{
	CFG* cfg;

	filter_back_edges () {}

	filter_back_edges (CFG* cfgs)
	: cfg (cfg)
	{
	}

	template <typename Edge>
	bool operator()(const Edge& e) const
	{
		// back edges have a gray target.
		return gray_color != cfg->cm[target(e, cfg->bs)];
	}
};

BB_list*
CFG::get_all_bbs_top_down ()
{
	typedef filtered_graph<Graph, filter_back_edges> DAG;

	renumber_vertex_indices ();

	// Create a new graph, without back edges.
	DAG fg (bs, filter_back_edges (this));

	// Do a topologic sort on the graph.
	vector<vertex_t> vertices;
	topological_sort(fg, back_inserter(vertices), color_map(cm));

	// Convert to a list of BBs
	BB_list* result = new BB_list;
	foreach (vertex_t v, vertices)
	{
		result->push_back (vb[v]);
	}

	return result;
}

BB_list*
CFG::get_all_bbs_bottom_up ()
{
	BB_list* result = get_all_bbs_top_down ();
	result->reverse ();
	return result;
}

void
CFG::tidy_up ()
{
	// TODO replace with worklist algorithm (beware, you cant remove a node
	// twice. BGL doesnt like it.
	bool repeat = true;
	while (repeat)
	{
		repeat = false;
		foreach (Basic_block* bb, *get_all_bbs ())
		{
			// Remove unreachable blocks (ie, no predecessors and are not the entry
			// block).
			if (isa<Entry_block> (bb) || isa<Exit_block> (bb))
				continue;

			// Don't remove a block with phi nodes.
			if (bb->get_phi_nodes ()->size () > 0)
				continue;

			// TODO: Dont remove infinite loops
//			if (bb->has_self_edge ())
//				continue;

			if (
				isa<Empty_block> (bb)
				|| bb->get_predecessors ()->size() == 0
				|| bb->get_successors ()->size() == 0)
			{
				assert (!isa<Branch_block> (bb)); // special cases?
				repeat = true;
				bb->remove ();
			}
		}
	}
}
