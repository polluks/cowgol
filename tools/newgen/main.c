#include "globals.h"
#include "parser.h"
#include <ctype.h>
#include <errno.h>

#if defined COWGOL
	#include "iburgcodes-coh.h"
#else
	#include "iburgcodes.h"
#endif

static Symbol* symbol_table = NULL;
static const char* SYM_REGISTER = "register";
static const char* SYM_REGCLASS = "register class";

static Rule* rules[500];
static int rulescount = 0;
static int registercount = 0;

static int maxdepth = 0;
static Node* pattern;

static const char* infilename;
FILE* outfp;
FILE* outhfp;

#if defined COWGOL
	#define DEREF "."
#else
	#define DEREF "->"
#endif

void* lookup_symbol(const char* name, const char* kind)
{
	Symbol* s = symbol_table;
	while (s)
	{
		if (strcmp(s->name, name) == 0)
		{
			if (kind && (s->kind != kind))
				fatal("symbol '%s' is not a %s", name, kind);
			return s;
		}
		s = s->next;
	}
	return NULL;
}

void* define_symbol(const char* name, const char* kind, size_t size)
{
	if (lookup_symbol(name, kind))
		fatal("symbol '%s' already exists", name);
	Symbol* s = calloc(size, 1);
	s->name = name;
	s->kind = kind;
	s->next = symbol_table;
	symbol_table = s;
	return s;
}

Register* define_register(const char* name)
{
	Register* reg = define_symbol(name, SYM_REGISTER, sizeof(Register));
	reg->compatible = reg->uses = reg->id = 1<<registercount;
	registercount++;
	return reg;
}

Register* lookup_register(const char* name)
{
	Register* reg = lookup_symbol(name, SYM_REGISTER);
	if (!reg)
		fatal("unknown register '%s'", name);
	return reg;
}

Register* lookup_register_by_id(reg_t id)
{
	Register* reg = (Register*) symbol_table;
	while (reg)
	{
		if ((reg->sym.kind == SYM_REGISTER) && (reg->id == id))
			return reg;
		reg = (Register*) reg->sym.next;
	}
	fatal("unknown register 0x%x", id);
}

reg_t lookup_register_or_class(const char* name)
{
	Symbol* s = lookup_symbol(name, NULL);
	if (s)
	{
		if (s->kind == SYM_REGISTER)
			return ((Register*)s)->id;
		if (s->kind == SYM_REGCLASS)
			return ((RegisterClass*)s)->reg;
	}
	fatal("'%s' is not a register or register class", name);
}

static reg_t find_conflicting_registers(reg_t id)
{
	reg_t conflicting = 0;
	Register* reg = (Register*) symbol_table;
	while (reg)
	{
		if ((reg->sym.kind == SYM_REGISTER) && (reg->id & id))
			conflicting |= reg->uses;
		reg = (Register*) reg->sym.next;
	}
	return conflicting;
}

void define_regclass(const char* name, reg_t reg)
{
	RegisterClass* rc = define_symbol(name, SYM_REGCLASS, sizeof(RegisterClass));
	rc->reg = reg;
}

int lookup_midcode(const char* name)
{
	/* TODO: these are not terminals, change the name */
	for (int i=0; i<sizeof(terminals)/sizeof(*terminals); i++)
	{
		const char* t = terminals[i];
		if (strcmp(name, t) == 0)
			return i; 
	}
	yyerror("unknown midcode '%s'", name);
	return 0;
}

Node* register_matcher(reg_t reg, Label* label)
{
	Node* n = calloc(sizeof(Node), 1);
	n->isregister = true;
	n->reg = reg;
	n->label = label;
	return n;
}

Node* tree_matcher(int midcode, Node* left, Node* right, Predicate* predicate, Label* label)
{
	Node* n = calloc(sizeof(Node), 1);
	n->isregister = false;
	n->midcode = midcode;
	n->left = left;
	n->right = right;
	n->predicate = predicate;
	n->label = label;
	return n;
}

Rule* rewriterule(int lineno, Node* pattern, Node* replacement)
{
	Rule* r = calloc(sizeof(Rule), 1);
	r->lineno = lineno;
	r->pattern = pattern;
	r->replacement = replacement;

	if (rulescount == (sizeof(rules)/sizeof(*rules)))
		yyerror("too many rules");
	rules[rulescount++] = r;
	return r;
}

Rule* genrule(int lineno, Node* pattern, reg_t result)
{
	Rule* r = calloc(sizeof(Rule), 1);
	r->lineno = lineno;
	r->pattern = pattern;
	r->result_reg = result;

	if (rulescount == (sizeof(rules)/sizeof(*rules)))
		yyerror("too many rules");
	rules[rulescount++] = r;
	return r;
}

static int collect_template_data(Node* template, Node* pattern, Label** last_label)
{
	if (!template)
		return 0;
	if (template->label)
	{
		template->label->next = *last_label;
		*last_label = template->label;
	}

	int cost = !(template->isregister) + !!(template->predicate);
	if (template->left)
	{
		if (!pattern->left)
		{
			Node* p = calloc(sizeof(Node), 1);
			pattern->left = p;
		}
		cost += collect_template_data(template->left, pattern->left, last_label);
	}
	if (template->right)
	{
		if (!pattern->right)
		{
			Node* p = calloc(sizeof(Node), 1);
			pattern->right = p;
		}
		cost += collect_template_data(template->right, pattern->right, last_label);
	}
	return cost;
}

static int sort_rule_cb(const void* left, const void* right)
{
	const Rule* const* r1 = left;
	const Rule* const* r2 = right;
	if ((*r1)->cost > (*r2)->cost)
		return -1;
	if ((*r1)->cost < (*r2)->cost)
		return 1;
	return 0;
}

static void calculate_pattern_size(Node* node)
{
	maxdepth++;
	if (node->left)
		calculate_pattern_size(node->left);
	if (node->right)
		calculate_pattern_size(node->right);
}

static Node* lookup_label(Node* node, const char* name)
{
	Node* f = NULL;
	if (!node)
		return f;
	else if (node->label && (strcmp(node->label->name, name) == 0))
		f = node;
	else
	{
		f = lookup_label(node->left, name);
		if (!f)
			f = lookup_label(node->right, name);
	}
	return f;
}

static void resolve_label_names(Rule* rule)
{
	Label* label = rule->first_label;
	while (label)
	{
		/* Check the name of this label doesn't conflict with any other
		 * on the rule. */

		Label* other = label->next;
		while (other)
		{
			if (strcmp(label->name, other->name) == 0)
				fatal("duplicate label '%s'", label->name);
			other = other->next;
		}

		label = label->next;
	}
}

static void copy_nodes(int* offset, Rule* rule, Node* template, Node* pattern)
{
	int thisoffset = *offset;
	if (template)
	{
		template->index = thisoffset;
		rule->nodes[thisoffset] = template;
		if (template->predicate)
			rule->has_predicates = true;
	}

	if (pattern->left)
	{
		(*offset)++;
		copy_nodes(offset, rule, template ? template->left : NULL, pattern->left);
	}
	if (pattern->right)
	{
		(*offset)++;
		copy_nodes(offset, rule, template ? template->right : NULL, pattern->right);
	}
}

static void sort_rules(void)
{
	pattern = calloc(sizeof(Node), 1);

	for (int i=0; i<rulescount; i++)
	{
		Rule* r = rules[i];
		r->cost = collect_template_data(r->pattern, pattern, &r->first_label);
	}

	qsort(rules, rulescount, sizeof(Rule*), sort_rule_cb);

	calculate_pattern_size(pattern);

	for (int i=0; i<rulescount; i++)
	{
		Rule* r = rules[i];
		r->nodes = calloc(maxdepth, sizeof(Node*));

		int offset = 0;
		copy_nodes(&offset, r, r->pattern, pattern);

		resolve_label_names(r);

		r->compatible_regs = 0;
		Register* reg = (Register*) symbol_table;
		while (reg)
		{
			if ((reg->sym.kind == SYM_REGISTER) && (r->result_reg & reg->id))
				r->compatible_regs |= reg->compatible;
			reg = (Register*) reg->sym.next;
		}
	}
}

static void print_upper(FILE* fp, const char* s)
{
	while (*s)
		fputc(toupper(*s++), fp);
}

static void dump_registers(void)
{
	#if defined COWGOL
		fprintf(outfp, "var registers: Register[] := {\n");
	#else
		fprintf(outfp, "const Register registers[] = {\n");
		fprintf(outhfp, "enum {\n");
	#endif

	Register* reg = (Register*) symbol_table;
	while(reg)
	{
		if (reg->sym.kind == SYM_REGISTER)
		{
			fprintf(outfp, "\t{ \"%s\", 0x%x, 0x%x, 0x%x, %d },\n",
				reg->sym.name, reg->id, reg->uses, reg->compatible, reg->isstacked);

			#if defined COWGOL
				fprintf(outhfp, "const REG_");
				print_upper(outhfp, reg->sym.name);
				fprintf(outhfp, " := 0x%x;\n", reg->id);
			#else
				fprintf(outhfp, "\tREG_");
				print_upper(outhfp, reg->sym.name);
				fprintf(outhfp, " = 0x%x,\n", reg->id);
			#endif
		}
		reg = (Register*) reg->sym.next;
	}
	#if defined COWGOL
		fprintf(outfp, "};\n");
	#else
		fprintf(outfp, "};\n");
		fprintf(outhfp, "};\n");
	#endif
}

static const char* operator_name(int operator)
{
	switch (operator)
	{
		case EQOP: return "==";
		case NEOP: return "!=";
		case LTOP: return "<";
		case LEOP: return "<=";
		case GTOP: return ">";
		case GEOP: return ">=";
	}
	assert(false);
}

static void print_lower(const char* s)
{
	while (*s)
		fputc(tolower(*s++), outfp);
}

static void print_predicate(int index, bool* first, Node* template, Predicate* predicate)
{
	while (predicate)
	{
		if (!*first)
		{
			#if defined COWGOL
				fprintf(outfp, " and");
			#else
				fprintf(outfp, " &&");
			#endif
		}
		*first = false;

		switch (predicate->operator)
		{
			case IS:
				#if defined COWGOL
					fprintf(outfp, " (is_%s([n + %d * @bytesof intptr].", predicate->u.callback, index);
				#else
					fprintf(outfp, " (is_%s(n[%d]->u.", predicate->u.callback, index);
				#endif
				print_lower(terminals[template->midcode]);
				fprintf(outfp, ".%s) != 0)", predicate->field);
				break;

			default:
				#if defined COWGOL
					fprintf(outfp, " ([n + %d * @bytesof intptr].", index);
				#else
					fprintf(outfp, " (n[%d]->u.", index);
				#endif
				print_lower(terminals[template->midcode]);
				fprintf(outfp, ".%s %s %d)",
					predicate->field,
					operator_name(predicate->operator),
					predicate->u.value);
				break;
		}

		predicate = predicate->next;
	}
}

static void create_match_predicates(void)
{
	#if defined COWGOL
		fprintf(outfp, "sub MatchPredicate(rule: uint8, n: [[Node]]): (matches: uint8)\n");
		fprintf(outfp, "matches := 0;\n");
		fprintf(outfp, "case rule is\n");
	#else
		fprintf(outfp, "bool match_predicate(uint8_t rule, Node** n) {\n");
		fprintf(outfp, "switch (rule) {\n");
	#endif

	for (int i=0; i<rulescount; i++)
	{
		Rule* r = rules[i];
		if (r->has_predicates)
		{
			#if defined COWGOL
				fprintf(outfp, "when %d:\n", i);
				fprintf(outfp, "if ");
			#else
				fprintf(outfp, "case %d:\n", i);
				fprintf(outfp, "\treturn");
			#endif

			bool first = true;
			for (int j=0; j<maxdepth; j++)
			{
				Node* n = r->nodes[j];
				if (n)
					print_predicate(j, &first, n, n->predicate);
			}
			#if defined COWGOL
				fprintf(outfp, " then matches := 1; end if;\n");
			#else
				fprintf(outfp, ";\n");
			#endif
		}
	}

	#if defined COWGOL
		fprintf(outfp, "end case;\n");
		fprintf(outfp, "end sub;\n");
	#else
		fprintf(outfp, "}\n");
		fprintf(outfp, "return false;\n");
		fprintf(outfp, "}\n");
	#endif
}

static void create_rules(void)
{
	#if defined COWGOL
		fprintf(outfp, "var codegen_rules: Rule[] := {\n");
	#else
		fprintf(outfp, "const Rule codegen_rules[] = {\n");
	#endif

	for (int i=0; i<rulescount; i++)
	{
		Rule* r = rules[i];
		fprintf(outfp, "\t{ ");

		uint8_t flags = 0;
		if (r->has_predicates)
			flags |= 0x01;
		if (r->replacement)
			flags |= 0x02;
		fprintf(outfp, "0x%02x, ", flags);

		fprintf(outfp, "0x%x, ", r->compatible_regs);
		fprintf(outfp, "0x%x, ", r->result_reg);
		fprintf(outfp, "0x%x, ", find_conflicting_registers(r->uses_regs));

		fprintf(outfp, "{ ");
		for (int j=0; j<maxdepth; j++)
		{
			Node* n = r->nodes[j];
			if (j)
				fprintf(outfp, ", ");
			if (n && n->isregister)
				fprintf(outfp, "%d", n->reg);
			else
				fprintf(outfp, "0");
		}
		fprintf(outfp, " }, ");

		fprintf(outfp, "{ ");
		for (int j=0; j<maxdepth; j++)
		{
			Node* n = r->nodes[j];
			fprintf(outfp, "%d, ", n ? n->midcode : 0);
		}
		fprintf(outfp, "}, ");

		uint8_t copymask = 1;
		uint8_t regmask = 0;
		for (int j=1; j<maxdepth; j++)
		{
			if (r->nodes[j])
			{
				copymask |= 1<<j;
				if (r->nodes[j]->isregister)
					regmask |= 1<<j;
			}
		}
		fprintf(outfp, "%d, %d ", copymask, regmask);
		fprintf(outfp, "}, ");

		#if defined COWGOL
			fprintf(outfp, "# %d\n", i);
		#else
			fprintf(outfp, "/* %d */\n", i);
		#endif
	}

	fprintf(outfp, "};\n");
}

static void print_complex_action(Rule* r, Element* e)
{
	if (e->next)
		print_complex_action(r, e->next);

	if (e->islabel)
	{
		if (e->text[0] == '$')
		{
			#if defined COWGOL
				fprintf(outfp, "selfreg");
			#else
				fprintf(outfp, "self" DEREF "produced_reg");
			#endif
		}
		else
		{
			Node* node = lookup_label(r->pattern, e->text);
			if (!node)
				fatal("nothing labelled '%s' at line %d", e->text, r->lineno);

			if (node->isregister)
			{
				#if defined COWGOL
					fprintf(outfp, "slots[%d].reg", node->index);
				#else
					fprintf(outfp, "self" DEREF "n[%d]" DEREF "produced_reg", node->index);
				#endif
			}
			else
			{
				#if defined COWGOL
					fprintf(outfp, "slots[%d].node" DEREF, node->index);
				#else
					fprintf(outfp, "self" DEREF "n[%d]" DEREF "u.", node->index);
				#endif
				print_lower(terminals[node->midcode]);
			}
		}
	}
	else
		fprintf(outfp, "%s", e->text);
}

static void print_line(int lineno)
{
	fprintf(outfp, "#line %d \"%s\"\n", lineno+1, infilename);
}

static void create_emitters(void)
{
	#if defined COWGOL
		fprintf(outfp, "sub EmitOneInstruction(rule: uint8, self: [Instruction])\n");
		fprintf(outfp, "record NodeSlot\n");
		fprintf(outfp, "\tnode: [Node];\n");
		fprintf(outfp, "\treg: RegId;\n");
		fprintf(outfp, "end record;\n");
		fprintf(outfp, "var slots: NodeSlot[%d];\n", maxdepth);
		fprintf(outfp, "var psrc := &self.n[0];\n");
		fprintf(outfp, "var pdest := &slots[0];\n");
		fprintf(outfp, "var i: uint8 := %d;\n", maxdepth);
		fprintf(outfp, "while i != 0 loop\n");
		fprintf(outfp, "\tpdest.node := [psrc];\n");
		fprintf(outfp, "\tif pdest.node != (0 as [Node]) then\n");
		fprintf(outfp, "\t\tpdest.reg := pdest.node.produced_reg;\n");
		fprintf(outfp, "\tend if;\n");
		fprintf(outfp, "\tpsrc := @next psrc;\n");
		fprintf(outfp, "\tpdest := @next pdest;\n");
		fprintf(outfp, "\ti := i - 1;\n");
		fprintf(outfp, "end loop;\n");
		fprintf(outfp, "var selfreg := self.produced_reg;\n");
		fprintf(outfp, "case rule is\n");
	#else
		fprintf(outfp, "void emit_one_instruction(uint8_t rule, Instruction* self) {\n");
		fprintf(outfp, "switch (rule) {\n");
	#endif

	for (int i=0; i<rulescount; i++)
	{
		Rule* r = rules[i];
		if (r->action)
		{
			#if defined COWGOL
				fprintf(outfp, "when %d:\n", i);
				fprintf(outfp, "sub emit_%d()\n", i);
			#else
				fprintf(outfp, "case %d: {\n", i);
			#endif


			print_line(r->lineno);

			Action* a = r->action;
			if (a)
			{
				if (a->iscomplex)
					print_complex_action(r, a->first_element);
				else
					fatal("simple actions not supported yet");
			}

			#if defined COWGOL
				fprintf(outfp, "\nend sub;\n");
				fprintf(outfp, "emit_%d();\n", i);
			#else
				fprintf(outfp, "\n} break;\n");
			#endif
		}
	}

	#if defined COWGOL
		fprintf(outfp, "end case;\n");
		fprintf(outfp, "end sub;\n");
	#else
		fprintf(outfp, "}\n");
		fprintf(outfp, "}\n");
	#endif
}

static void emit_replacement(Rule* rule, Node* pattern, Node* replacement)
{
	if (replacement->isregister)
	{
		Node* node = lookup_label(pattern, replacement->label->name);
		if (!node)
			fatal("nothing labelled '%s' at line %d",
				replacement->label->name, rule->lineno);

		fprintf(outfp, "n[%d]", node->index);
	}
	else
	{
		fprintf(outfp, "mid_");
		print_lower(terminals[replacement->midcode]);
		fprintf(outfp, "(");

		if (replacement->left)
		{
			emit_replacement(rule, pattern, replacement->left);
			if (replacement->right)
			{
				fprintf(outfp, ", ");
				emit_replacement(rule, pattern, replacement->right);
			}
		}
		fprintf(outfp, ")");
	}
}

static void create_rewriters(void)
{
	#if defined COWGOL
		fprintf(outfp, "sub RewriteNode(rule: uint8, n: [[Node]]): (result: [Node])\n");
		fprintf(outfp, "case rule is\n");
	#else
		fprintf(outfp, "Node* rewrite_node(uint8_t rule, Node** n) {\n");
		fprintf(outfp, "switch (rule) {\n");
	#endif

	for (int i=0; i<rulescount; i++)
	{
		Rule* r = rules[i];
		if (r->replacement)
		{
			#if defined COWGOL
				fprintf(outfp, "when %d:\n", i);
				fprintf(outfp, "result :=\n");
			#else
				fprintf(outfp, "case %d:\n", i);
				fprintf(outfp, "\treturn\n");
			#endif

			print_line(r->lineno);
			emit_replacement(r, r->pattern, r->replacement);
			fprintf(outfp, ";\n");
		}
	}

	#if defined COWGOL
		fprintf(outfp, "end case;\n");
		fprintf(outfp, "end sub;\n");
	#else
		fprintf(outfp, "}\n");
		fprintf(outfp, "return NULL;\n");
		fprintf(outfp, "}\n");
	#endif
}

static void walk_matcher_tree(int* offset, Node* pattern)
{
	int thisoffset = *offset;
	#if defined COWGOL
		fprintf(outfp, "\t[matchbuf+%d] := [n + %d*@bytesof intptr].op;\n", thisoffset, thisoffset);
	#else
		fprintf(outfp, "\tmatchbuf[%d] = n[%d]->op;\n", thisoffset, thisoffset);
	#endif

	if (pattern->left)
	{
		(*offset)++;
		#if defined COWGOL
			fprintf(outfp, "\t[n + %d*@bytesof intptr] := [n + %d*@bytesof intptr].left;\n", *offset, thisoffset);
			fprintf(outfp, "\tif [n + %d*@bytesof intptr] != (0 as [Node]) then\n", *offset);
			walk_matcher_tree(offset, pattern->left);
			fprintf(outfp, "\tend if;\n");
		#else
			fprintf(outfp, "\tn[%d] = n[%d]->left;\n", *offset, thisoffset);
			fprintf(outfp, "\tif (n[%d]) {\n", *offset);
			walk_matcher_tree(offset, pattern->left);
			fprintf(outfp, "\t}\n");
		#endif
	}
	if (pattern->right)
	{
		(*offset)++;
		#if defined COWGOL
			fprintf(outfp, "\t[n + %d*@bytesof intptr] := [n + %d*@bytesof intptr].right;\n", *offset, thisoffset);
			fprintf(outfp, "\tif [n + %d*@bytesof intptr] != (0 as [Node]) then\n", *offset);
			walk_matcher_tree(offset, pattern->right);
			fprintf(outfp, "\tend if;\n");
		#else
			fprintf(outfp, "\tn[%d] = n[%d]->right;\n", *offset, thisoffset);
			fprintf(outfp, "\tif (n[%d]) {\n", *offset);
			walk_matcher_tree(offset, pattern->right);
			fprintf(outfp, "\t}\n");
		#endif
	}
}

static void create_matcher(void)
{
	#if defined COWGOL
		fprintf(outfp, "sub PopulateMatchBuffer(insn: [Instruction], n: [[Node]], matchbuf: [uint8])\n");
	#else
		fprintf(outfp, "void populate_match_buffer(Instruction* insn, Node** n, uint8_t* matchbuf) {\n");
	#endif

	int offset = 0;
	walk_matcher_tree(&offset, pattern);

	#if defined COWGOL
		fprintf(outfp, "end sub;\n");
	#else
		fprintf(outfp, "};\n");
	#endif

}

int main(int argc, const char* argv[])
{
	if (argc != 4)
		fatal("syntax: newgen <inputfile> <output c file> <output h file>");

	infilename = argv[1];
	FILE* infp = fopen(infilename, "r");
	if (!infp)
		fatal("cannot open input file '%s': %s", infilename, strerror(errno));

	outfp = fopen(argv[2], "w");
	if (!outfp)
		fatal("cannot open output C file '%s': %s", argv[2], strerror(errno));

	outhfp = fopen(argv[3], "w");
	if (!outhfp)
		fatal("cannot open output H file '%s': %s", argv[3], strerror(errno));


	include_file(open_file(infp));
	parse();

	sort_rules();

	#if defined COWGOL
		fprintf(outhfp, "const INSTRUCTION_TEMPLATE_DEPTH := %d;\n", maxdepth);
		fprintf(outhfp, "const INSTRUCTION_TEMPLATE_COUNT := %d;\n", rulescount);
		fprintf(outhfp, "const REGISTER_COUNT := %d;\n", registercount);
		fprintf(outhfp, "typedef RegId := int(0, 0x%x);\n", (1<<registercount) - 1);
	#else
		fprintf(outhfp, "#ifndef NEWGEN_H\n");
		fprintf(outhfp, "#define NEWGEN_H\n");
		fprintf(outhfp, "#define INSTRUCTION_TEMPLATE_DEPTH %d\n", maxdepth);
		fprintf(outhfp, "#define INSTRUCTION_TEMPLATE_COUNT %d\n", rulescount);
		fprintf(outhfp, "#define REGISTER_COUNT %d\n", registercount);
	#endif

	dump_registers();
	create_match_predicates();
	create_emitters();
	create_rewriters();
	create_rules();
	create_matcher();

	#if !defined COWGOL
		fprintf(outhfp, "#endif\n");
	#endif

	return errcnt>0;
}
