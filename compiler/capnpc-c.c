/* capnpc-c.c
 *
 * Copyright (C) 2013 James McKaskill
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "schema.capnp.h"
#include "str.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct value {
	struct Type t;
	const char *tname;
	struct str tname_buf;
	struct Value v;
	capn_ptr ptrval;
	int64_t intval;
};

struct field {
	struct Field f;
	struct value v;
	struct node *group;
};

struct node {
	struct capn_tree hdr;
	struct Node n;
	struct node *next;
	struct node *file_nodes, *next_file_node;
	struct str name;
	struct field *fields;
};

static struct str SRC = STR_INIT, HDR = STR_INIT;
static struct capn g_valcapn;
static struct capn_segment g_valseg;
static int g_valc;
static int g_val0used, g_nullused;

static int g_fieldgetset = 0;

static struct capn_tree *g_node_tree;

static struct node *find_node_mayfail(uint64_t id) {
	struct node *s = (struct node*) g_node_tree;
	while (s && s->n.id != id) {
		s = (struct node*) s->hdr.link[s->n.id < id];
	}
	return s;
}

static struct node *find_node(uint64_t id)
{
	struct node *s = find_node_mayfail(id);
	if (s == NULL) {
		fprintf(stderr, "cant find node with id 0x%x%x\n", (uint32_t) (id >> 32), (uint32_t) id);
		exit(2);
	}
	return s;
}

static void insert_node(struct node *s) {
	struct capn_tree **x = &g_node_tree;
	while (*x) {
		s->hdr.parent = *x;
		x = &(*x)->link[((struct node*)*x)->n.id < s->n.id];
	}
	*x = &s->hdr;
	g_node_tree = capn_tree_insert(g_node_tree, &s->hdr);
}

/* resolve_names recursively follows the nestedNodes tree in order to
 * set node->name.
 * It also builds up the list of nodes within a file (file_nodes and
 * next_file_node). */
static void resolve_names(struct str *b, struct node *n, capn_text name, struct node *file) {
	int i, sz = b->len;
	str_add(b, name.str, name.len);
	str_add(&n->name, b->str, b->len);
	str_add(b, "_", 1);

	for (i = capn_len(n->n.nestedNodes)-1; i >= 0; i--) {
		struct Node_NestedNode nest;
		get_Node_NestedNode(&nest, n->n.nestedNodes, i);
		struct node *nn = find_node(nest.id);
		if (nn != NULL) {
			resolve_names(b, nn, nest.name, file);
		}
	}

	if (n->n.which == Node__struct) {
		for (i = capn_len(n->n._struct.fields)-1; i >= 0; i--) {
			if (n->fields[i].group) {
				resolve_names(b, n->fields[i].group, n->fields[i].f.name, file);
			}
		}
	}

	if (n->n.which != Node__struct || !n->n._struct.isGroup) {
		n->next_file_node = file->file_nodes;
		file->file_nodes = n;
	}

	str_setlen(b, sz);
}

static void define_enum(struct node *n) {
	int i;

	str_addf(&HDR, "\nenum %s {", n->name.str);
	for (i = 0; i < capn_len(n->n._enum.enumerants); i++) {
		struct Enumerant e;
		get_Enumerant(&e, n->n._enum.enumerants, i);
		if (i) {
			str_addf(&HDR, ",");
		}
		str_addf(&HDR, "\n\t%s_%s = %d", n->name.str, e.name.str, i);
	}
	str_addf(&HDR, "\n};\n");
}

static void decode_value(struct value* v, Type_ptr type, Value_ptr value, const char *symbol) {
	struct Type list_type;
	memset(v, 0, sizeof(*v));
	read_Type(&v->t, type);
	read_Value(&v->v, value);

	switch (v->t.which) {
	case Type__void:
		v->tname = "void";
		break;
	case Type__bool:
		v->tname = "unsigned";
		break;
	case Type_int8:
		v->tname = "int8_t";
		break;
	case Type_int16:
		v->tname = "int16_t";
		break;
	case Type_int32:
		v->tname = "int32_t";
		break;
	case Type_int64:
		v->tname = "int64_t";
		break;
	case Type_uint8:
		v->tname = "uint8_t";
		break;
	case Type_uint16:
		v->tname = "uint16_t";
		break;
	case Type_uint32:
		v->tname = "uint32_t";
		break;
	case Type_uint64:
		v->tname = "uint64_t";
		break;
	case Type_float32:
		v->tname = "float";
		break;
	case Type_float64:
		v->tname = "double";
		break;
	case Type_text:
		v->tname = "capn_text";
		break;
	case Type_data:
		v->tname = "capn_data";
		break;
	case Type__enum:
		v->tname = strf(&v->tname_buf, "enum %s", find_node(v->t._enum.typeId)->name.str);
		break;
	case Type__struct:
	case Type__interface:
		v->tname = strf(&v->tname_buf, "%s_ptr", find_node(v->t._struct.typeId)->name.str);
		break;
	case Type_anyPointer:
		v->tname = "capn_ptr";
		break;
	case Type__list:
		read_Type(&list_type, v->t._list.elementType);

		switch (list_type.which) {
		case Type__void:
			v->tname = "capn_ptr";
			break;
		case Type__bool:
			v->tname = "capn_list1";
			break;
		case Type_int8:
		case Type_uint8:
			v->tname = "capn_list8";
			break;
		case Type_int16:
		case Type_uint16:
		case Type__enum:
			v->tname = "capn_list16";
			break;
		case Type_int32:
		case Type_uint32:
		case Type_float32:
			v->tname = "capn_list32";
			break;
		case Type_int64:
		case Type_uint64:
		case Type_float64:
			v->tname = "capn_list64";
			break;
		case Type_text:
		case Type_data:
		case Type_anyPointer:
		case Type__list:
			v->tname = "capn_ptr";
			break;
		case Type__struct:
		case Type__interface:
			v->tname = strf(&v->tname_buf, "%s_list", find_node(list_type._struct.typeId)->name.str);
			break;
		}
	}

	switch (v->v.which) {
	case Value__bool:
		v->intval = v->v._bool;
		break;
	case Value_int8:
	case Value_uint8:
		v->intval = v->v.int8;
		break;
	case Value_int16:
	case Value_uint16:
	case Value__enum:
		v->intval = v->v.int16;
		break;
	case Value_int32:
	case Value_uint32:
	case Value_float32:
		v->intval = v->v.int32;
		break;
	case Value_int64:
	case Value_float64:
	case Value_uint64:
		v->intval = v->v.int64;
		break;
	case Value_text:
		if (v->v.text.len) {
			capn_ptr p = capn_root(&g_valcapn);
			if (capn_set_text(p, 0, v->v.text)) {
				fprintf(stderr, "failed to copy text\n");
				exit(2);
			}
			p = capn_getp(p, 0, 1);
			if (!p.type)
				break;

			v->ptrval = p;

			bool symbol_provided = symbol;
			if (!symbol) {
				static struct str buf = STR_INIT;
				v->intval = ++g_valc;
				symbol = strf(&buf, "capn_val%d", (int) v->intval);
			}

			str_addf(&SRC, "%scapn_text %s = {%d,(char*)&capn_buf[%d],(struct capn_segment*)&capn_seg};\n",
					symbol_provided ? "" : "static ",
					symbol,
					p.len-1,
					(int) (p.data-p.seg->data-8));
		}
		break;

	case Value_data:
	case Value__struct:
	case Value_anyPointer:
	case Value__list:
		if (v->v.anyPointer.type) {
			capn_ptr p = capn_root(&g_valcapn);
			if (capn_setp(p, 0, v->v.anyPointer)) {
				fprintf(stderr, "failed to copy object\n");
				exit(2);
			}
			p = capn_getp(p, 0, 1);
			if (!p.type)
				break;

			v->ptrval = p;

			bool symbol_provided = symbol;
			if (!symbol) {
				static struct str buf = STR_INIT;
				v->intval = ++g_valc;
				symbol = strf(&buf, "capn_val%d", (int) v->intval);
			}

			str_addf(&SRC, "%s%s %s = {", symbol_provided ? "" : "static ", v->tname, symbol);
			if (strcmp(v->tname, "capn_ptr"))
				str_addf(&SRC, "{");

			str_addf(&SRC, "%d,%d,%d,%d,%d,%d,%d,(char*)&capn_buf[%d],(struct capn_segment*)&capn_seg",
					p.type,
					p.has_ptr_tag,
					p.is_list_member,
					p.is_composite_list,
					p.datasz,
					p.ptrs,
					p.len,
					(int) (p.data-p.seg->data-8));

			if (strcmp(v->tname, "capn_ptr"))
				str_addf(&SRC, "}");

			str_addf(&SRC, "};\n");
		}
		break;

	case Value__interface:
	case Value__void:
		break;
	}
}

static void define_const(struct node *n) {
	struct value v;
	decode_value(&v, n->n._const.type, n->n._const.value, n->name.str);

	switch (v.v.which) {
	case Value__bool:
	case Value_int8:
	case Value_int16:
	case Value_int32:
		str_addf(&HDR, "extern %s %s;\n", v.tname, n->name.str);
		str_addf(&SRC, "%s %s = %d;\n", v.tname, n->name.str, (int) v.intval);
		break;

	case Value_uint8:
		str_addf(&HDR, "extern %s %s;\n", v.tname, n->name.str);
		str_addf(&SRC, "%s %s = %u;\n", v.tname, n->name.str, (uint8_t) v.intval);
		break;

	case Value_uint16:
		str_addf(&HDR, "extern %s %s;\n", v.tname, n->name.str);
		str_addf(&SRC, "%s %s = %u;\n", v.tname, n->name.str, (uint16_t) v.intval);
		break;

	case Value_uint32:
		str_addf(&HDR, "extern %s %s;\n", v.tname, n->name.str);
		str_addf(&SRC, "%s %s = %uu;\n", v.tname, n->name.str, (uint32_t) v.intval);
		break;

	case Value__enum:
		str_addf(&HDR, "extern %s %s;\n", v.tname, n->name.str);
		str_addf(&SRC, "%s %s = (%s) %uu;\n", v.tname, n->name.str, v.tname, (uint32_t) v.intval);
		break;

	case Value_int64:
	case Value_uint64:
		str_addf(&HDR, "extern %s %s;\n", v.tname, n->name.str);
		str_addf(&SRC, "%s %s = ((uint64_t) %#xu << 32) | %#xu;\n", v.tname, n->name.str,
				(uint32_t) (v.intval >> 32), (uint32_t) v.intval);
		break;

	case Value_float32:
		str_addf(&HDR, "extern union capn_conv_f32 %s;\n", n->name.str);
		str_addf(&SRC, "union capn_conv_f32 %s = {%#xu};\n", n->name.str, (uint32_t) v.intval);
		break;

	case Value_float64:
		str_addf(&HDR, "extern union capn_conv_f64 %s;\n", n->name.str);
		str_addf(&SRC, "union capn_conv_f64 %s = {((uint64_t) %#xu << 32) | %#xu};\n",
				n->name.str, (uint32_t) (v.intval >> 32), (uint32_t) v.intval);
		break;

	case Value_text:
	case Value_data:
	case Value__struct:
	case Value_anyPointer:
	case Value__list:
		str_addf(&HDR, "extern %s %s;\n", v.tname, n->name.str);
		if (!v.ptrval.type) {
			str_addf(&SRC, "%s %s;\n", v.tname, n->name.str);
		}
		break;

	case Value__interface:
	case Value__void:
		break;
	}

	str_release(&v.tname_buf);
}

static void decode_field(struct field *fields, Field_list l, int i) {
	struct field f;
	memset(&f, 0, sizeof(f));
	get_Field(&f.f, l, i);

	if (f.f.codeOrder >= capn_len(l)) {
		fprintf(stderr, "unexpectedly large code order %d >= %d\n", f.f.codeOrder, capn_len(l));
		exit(3);
	}

	if (f.f.which == Field_group) {
		f.group = find_node(f.f.group.typeId);
	}

	memcpy(&fields[f.f.codeOrder], &f, sizeof(f));
}

static const char *xor_member(struct field *f) {
	static struct str buf = STR_INIT;

	if (f->v.intval) {
		switch (f->v.v.which) {
		case Value_int8:
		case Value_int16:
		case Value_int32:
			return strf(&buf, " ^ %d", (int32_t) f->v.intval);

		case Value_uint8:
			return strf(&buf, " ^ %uu", (uint8_t) f->v.intval);

		case Value_uint16:
		case Value__enum:
			return strf(&buf, " ^ %uu", (uint16_t) f->v.intval);

		case Value_uint32:
			return strf(&buf, " ^ %uu", (uint32_t) f->v.intval);

		case Value_float32:
			return strf(&buf, " ^ %#xu", (uint32_t) f->v.intval);

		case Value_int64:
			return strf(&buf, " ^ ((int64_t)((uint64_t) %#xu << 32) ^ %#xu)",
						(uint32_t) (f->v.intval >> 32), (uint32_t) f->v.intval);
		case Value_uint64:
		case Value_float64:
			return strf(&buf, " ^ ((uint64_t) %#xu << 32) ^ %#xu",
					(uint32_t) (f->v.intval >> 32), (uint32_t) f->v.intval);

		default:
			return "";
		}
	} else {
		return "";
	}
}

static const char *ptr_member(struct field *f, const char *var) {
	static struct str buf = STR_INIT;
	if (!strcmp(f->v.tname, "capn_ptr")) {
		return var;
	} else if (var[0] == '*') {
		return strf(&buf, "%s->p", var+1);
	} else {
		return strf(&buf, "%s.p", var);
	}
}

static void set_member(struct str *func, struct field *f, const char *ptr, const char *tab, const char *var) {
	const char *xor = xor_member(f);
	const char *pvar = ptr_member(f, var);

	if (f->v.t.which == Type__void)
		return;

	str_add(func, tab, -1);

	switch (f->v.t.which) {
	case Type__bool:
		str_addf(func, "capn_write1(%s, %d, %s != %d);\n", ptr, f->f.slot.offset, var, (int) f->v.intval);
		break;
	case Type_int8:
		str_addf(func, "capn_write8(%s, %d, (uint8_t) (%s%s));\n", ptr, f->f.slot.offset, var, xor);
		break;
	case Type_int16:
	case Type__enum:
		str_addf(func, "capn_write16(%s, %d, (uint16_t) (%s%s));\n", ptr, 2*f->f.slot.offset, var, xor);
		break;
	case Type_int32:
		str_addf(func, "capn_write32(%s, %d, (uint32_t) (%s%s));\n", ptr, 4*f->f.slot.offset, var, xor);
		break;
	case Type_int64:
		str_addf(func, "capn_write64(%s, %d, (uint64_t) (%s%s));\n", ptr, 8*f->f.slot.offset, var, xor);
		break;
	case Type_uint8:
		str_addf(func, "capn_write8(%s, %d, %s%s);\n", ptr, f->f.slot.offset, var, xor);
		break;
	case Type_uint16:
		str_addf(func, "capn_write16(%s, %d, %s%s);\n", ptr, 2*f->f.slot.offset, var, xor);
		break;
	case Type_uint32:
		str_addf(func, "capn_write32(%s, %d, %s%s);\n", ptr, 4*f->f.slot.offset, var, xor);
		break;
	case Type_float32:
		str_addf(func, "capn_write32(%s, %d, capn_from_f32(%s)%s);\n", ptr, 4*f->f.slot.offset, var, xor);
		break;
	case Type_uint64:
		str_addf(func, "capn_write64(%s, %d, %s%s);\n", ptr, 8*f->f.slot.offset, var, xor);
		break;
	case Type_float64:
		str_addf(func, "capn_write64(%s, %d, capn_from_f64(%s)%s);\n", ptr, 8*f->f.slot.offset, var, xor);
		break;
	case Type_text:
		if (f->v.ptrval.type) {
			g_val0used = 1;
			str_addf(func, "capn_set_text(%s, %d, (%s.str != capn_val%d.str) ? %s : capn_val0);\n",
					ptr, f->f.slot.offset, var, (int)f->v.intval, var);
		} else {
			str_addf(func, "capn_set_text(%s, %d, %s);\n",
					ptr, f->f.slot.offset, var);
		}
		break;
	case Type_data:
	case Type__struct:
	case Type__interface:
	case Type__list:
	case Type_anyPointer:
		if (!f->v.intval) {
			str_addf(func, "capn_setp(%s, %d, %s);\n",
					ptr, f->f.slot.offset, pvar);
		} else if (!strcmp(f->v.tname, "capn_ptr")) {
			g_nullused = 1;
			str_addf(func, "capn_setp(%s, %d, (%s.data != capn_val%d.data) ? %s : capn_null);\n",
					ptr, f->f.slot.offset, pvar, (int)f->v.intval, pvar);
		} else {
			g_nullused = 1;
			str_addf(func, "capn_setp(%s, %d, (%s.data != capn_val%d.p.data) ? %s : capn_null);\n",
					ptr, f->f.slot.offset, pvar, (int)f->v.intval, pvar);
		}
		break;
	default:
		break;
	}
}

static void get_member(struct str *func, struct field *f, const char *ptr, const char *tab, const char *var) {
	const char *xor = xor_member(f);
	const char *pvar = ptr_member(f, var);

	if (f->v.t.which == Type__void)
		return;

	str_add(func, tab, -1);

	switch (f->v.t.which) {
	case Type__bool:
		str_addf(func, "%s = (capn_read8(%s, %d) & %d) != %d;\n",
				var, ptr, f->f.slot.offset/8, 1 << (f->f.slot.offset%8), (int)f->v.intval);
		return;
	case Type_int8:
		str_addf(func, "%s = (int8_t) ((int8_t)capn_read8(%s, %d))%s;\n", var, ptr, f->f.slot.offset, xor);
		return;
	case Type_int16:
		str_addf(func, "%s = (int16_t) ((int16_t)capn_read16(%s, %d))%s;\n", var, ptr, 2*f->f.slot.offset, xor);
		return;
	case Type_int32:
		str_addf(func, "%s = (int32_t) ((int32_t)capn_read32(%s, %d))%s;\n", var, ptr, 4*f->f.slot.offset, xor);
		return;
	case Type_int64:
		str_addf(func, "%s = (int64_t) ((int64_t)(capn_read64(%s, %d))%s);\n", var, ptr, 8*f->f.slot.offset, xor);
		return;
	case Type_uint8:
		str_addf(func, "%s = capn_read8(%s, %d)%s;\n", var, ptr, f->f.slot.offset, xor);
		return;
	case Type_uint16:
		str_addf(func, "%s = capn_read16(%s, %d)%s;\n", var, ptr, 2*f->f.slot.offset, xor);
		return;
	case Type_uint32:
		str_addf(func, "%s = capn_read32(%s, %d)%s;\n", var, ptr, 4*f->f.slot.offset, xor);
		return;
	case Type_uint64:
		str_addf(func, "%s = capn_read64(%s, %d)%s;\n", var, ptr, 8*f->f.slot.offset, xor);
		return;
	case Type_float32:
		str_addf(func, "%s = capn_to_f32(capn_read32(%s, %d)%s);\n", var, ptr, 4*f->f.slot.offset, xor);
		return;
	case Type_float64:
		str_addf(func, "%s = capn_to_f64(capn_read64(%s, %d)%s);\n", var, ptr, 8*f->f.slot.offset, xor);
		return;
	case Type__enum:
		str_addf(func, "%s = (%s)(int) capn_read16(%s, %d)%s;\n", var, f->v.tname, ptr, 2*f->f.slot.offset, xor);
		return;
	case Type_text:
		if (!f->v.intval)
			g_val0used = 1;
		str_addf(func, "%s = capn_get_text(%s, %d, capn_val%d);\n", var, ptr, f->f.slot.offset, (int)f->v.intval);
		return;

	case Type_data:
		str_addf(func, "%s = capn_get_data(%s, %d);\n", var, ptr, f->f.slot.offset);
		break;
	case Type__struct:
	case Type__interface:
	case Type_anyPointer:
	case Type__list:
		str_addf(func, "%s = capn_getp(%s, %d, 0);\n", pvar, ptr, f->f.slot.offset);
		break;
	default:
		return;
	}

	if (f->v.intval) {
		str_addf(func, "%sif (!%s.type) {\n", tab, pvar);
		str_addf(func, "%s\t%s = capn_val%d;\n", tab, var, (int)f->v.intval);
		str_addf(func, "%s}\n", tab);
	}
}

struct strings {
	struct str ftab;
	struct str dtab;
	struct str get;
	struct str set;
	struct str enums;
	struct str decl;
	struct str var;
	struct str pub_get;
	struct str pub_get_header;
	struct str pub_set;
	struct str pub_set_header;
};

static const char *field_name(struct field *f) {
	static struct str buf = STR_INIT;
	static const char *reserved[] = {
		/* C++11 reserved words */
		"alignas", "alignof", "and", "and_eq", "asm",
		"auto", "bitand", "bitor", "bool", "break",
		"case", "catch", "char", "char16_t", "char32_t",
		"class", "compl", "const", "constexpr", "const_cast",
		"continue", "decltype", "default", "delete", "do",
		"double", "dynamic_cast", "else", "enum", "explicit",
		"export", "extern", "false", "float", "for",
		"friend", "goto", "if", "inline", "int",
		"long", "mutable", "namespace", "new", "noexcept",
		"not", "not_eq", "nullptr", "operator", "or",
		"or_eq", "private", "protected", "public", "register",
		"reinterpret_cast", "return", "short", "signed", "sizeof",
		"static", "static_assert", "static_cast", "struct", "switch",
		"template", "this", "thread_local", "throw", "true",
		"try", "typedef", "typeid", "typename", "union",
		"unsigned", "using", "virtual", "void", "volatile",
		"wchar_t", "while", "xor", "xor_eq",
		/* COM reserved words */
		"interface", "module", "import",
		/* capn reserved otherwise Value_ptr enum and type collide */
		"ptr", "list",
		/* C11 keywords not reserved in C++ */
		"restrict", "_Alignas", "_Alignof", "_Atomic", "_Bool",
		"_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
		"_Thread_local",
	};

	size_t i;
	const char *s = f->f.name.str;
	for (i = 0; i < sizeof(reserved)/sizeof(reserved[0]); i++) {
		if (!strcmp(s, reserved[i])) {
			return strf(&buf, "_%s", s);
		}
	}

	return s;
}

static void union_block(struct strings *s, struct field *f) {
	static struct str buf = STR_INIT;
	str_add(&s->ftab, "\t", -1);
	set_member(&s->set, f, "p.p", s->ftab.str, strf(&buf, "%s%s", s->var.str, field_name(f)));
	get_member(&s->get, f, "p.p", s->ftab.str, strf(&buf, "%s%s", s->var.str, field_name(f)));
	str_addf(&s->set, "%sbreak;\n", s->ftab.str);
	str_addf(&s->get, "%sbreak;\n", s->ftab.str);
	str_setlen(&s->ftab, s->ftab.len-1);
}

static int in_union(struct field *f) {
	return f->f.discriminantValue != 0xFFFF;
}

static void union_cases(struct strings *s, struct node *n, struct field *first_field, int mask) {
	struct field *f, *u = NULL;

	for (f = first_field; f < n->fields + capn_len(n->n._struct.fields) && in_union(f); f++) {

		if (f->f.which != Field_slot)
			continue;
		if (f->v.ptrval.type || f->v.intval)
			continue;
		if ((mask & (1 << f->v.t.which)) == 0)
			continue;

		u = f;
		str_addf(&s->set, "%scase %s_%s:\n", s->ftab.str, n->name.str, field_name(f));
		str_addf(&s->get, "%scase %s_%s:\n", s->ftab.str, n->name.str, field_name(f));
	}

	if (u)
		union_block(s, u);
}

static void declare_slot(struct strings *s, struct field *f) {
	switch (f->v.t.which) {
	case Type__void:
		break;
	case Type__bool:
		str_addf(&s->decl, "%s%s %s : 1;\n", s->dtab.str, f->v.tname, field_name(f));
		break;
	default:
		str_addf(&s->decl, "%s%s %s;\n", s->dtab.str, f->v.tname, field_name(f));
		break;
	}
}

static void define_group(struct strings *s, struct node *n, const char *group_name);

static void do_union(struct strings *s, struct node *n, struct field *first_field, const char *union_name) {
	int tagoff = 2 * n->n._struct.discriminantOffset;
	struct field *f;
	static struct str tag = STR_INIT;
	struct str enums = STR_INIT;

	str_reset(&tag);

	if (union_name) {
		str_addf(&tag, "%.*s_which", s->var.len - 1, s->var.str);
		str_addf(&enums, "enum %s_which {", n->name.str);
		str_addf(&s->decl, "%senum %s_which %s_which;\n", s->dtab.str, n->name.str, union_name);
		str_addf(&s->get, "%s%s = (enum %s_which)(int) capn_read16(p.p, %d);\n",
				s->ftab.str, tag.str, n->name.str, tagoff);
	} else {
		str_addf(&tag, "%swhich", s->var.str);
		str_addf(&enums, "enum %s_which {", n->name.str);
		str_addf(&s->decl, "%senum %s_which which;\n", s->dtab.str, n->name.str);
		str_addf(&s->get, "%s%s = (enum %s_which)(int) capn_read16(p.p, %d);\n",
				s->ftab.str, tag.str, n->name.str, tagoff);
	}

	str_addf(&s->set, "%scapn_write16(p.p, %d, %s);\n", s->ftab.str, tagoff, tag.str);
	str_addf(&s->set, "%sswitch (%s) {\n", s->ftab.str, tag.str);
	str_addf(&s->get, "%sswitch (%s) {\n", s->ftab.str, tag.str);

	/* if we have a bunch of the same C type with zero defaults, we
	 * only need to emit one switch block as the layout will line up
	 * in the C union */
	union_cases(s, n, first_field, (1 << Type__bool));
	union_cases(s, n, first_field, (1 << Type_int8) | (1 << Type_uint8));
	union_cases(s, n, first_field, (1 << Type_int16) | (1 << Type_uint16) | (1 << Type__enum));
	union_cases(s, n, first_field, (1 << Type_int32) | (1 << Type_uint32) | (1 << Type_float32));
	union_cases(s, n, first_field, (1 << Type_int64) | (1 << Type_uint64) | (1 << Type_float64));
	union_cases(s, n, first_field, (1 << Type_text));
	union_cases(s, n, first_field, (1 << Type_data));
	union_cases(s, n, first_field, (1 << Type__struct) | (1 << Type__interface) | (1 << Type_anyPointer) | (1 << Type__list));

	str_addf(&s->decl, "%scapnp_nowarn union {\n", s->dtab.str);
	str_add(&s->dtab, "\t", -1);

	/* when we have defaults or groups we have to emit each case seperately */
	for (f = first_field; f < n->fields + capn_len(n->n._struct.fields) && in_union(f); f++) {
		if (f > first_field) {
			str_addf(&enums, ",");
		}

		str_addf(&enums, "\n\t%s_%s = %d", n->name.str, field_name(f), f->f.discriminantValue);

		switch (f->f.which) {
		case Field_group:
			str_addf(&s->get, "%scase %s_%s:\n", s->ftab.str, n->name.str, field_name(f));
			str_addf(&s->set, "%scase %s_%s:\n", s->ftab.str, n->name.str, field_name(f));
			str_add(&s->ftab, "\t", -1);
			define_group(s, f->group, field_name(f));
			str_addf(&s->get, "%sbreak;\n", s->ftab.str);
			str_addf(&s->set, "%sbreak;\n", s->ftab.str);
			str_setlen(&s->ftab, s->ftab.len-1);
			break;

		case Field_slot:
			declare_slot(s, f);
			if (f->v.ptrval.type || f->v.intval) {
				str_addf(&s->get, "%scase %s_%s:\n", s->ftab.str, n->name.str, field_name(f));
				str_addf(&s->set, "%scase %s_%s:\n", s->ftab.str, n->name.str, field_name(f));
				union_block(s, f);
			}
			break;

		default:
			break;
		}
	}

	str_setlen(&s->dtab, s->dtab.len - 1);

	if (union_name) {
		str_addf(&s->decl, "%s} %s;\n", s->dtab.str, union_name);
	} else {
		str_addf(&s->decl, "%s};\n", s->dtab.str);
	}

	str_addf(&s->get, "%sdefault:\n%s\tbreak;\n%s}\n", s->ftab.str, s->ftab.str, s->ftab.str);
	str_addf(&s->set, "%sdefault:\n%s\tbreak;\n%s}\n", s->ftab.str, s->ftab.str, s->ftab.str);

	str_addf(&enums, "\n};\n");
	str_add(&s->enums, enums.str, enums.len);
	str_release(&enums);
}

static void define_field(struct strings *s, struct field *f) {
	static struct str buf = STR_INIT;

	switch (f->f.which) {
	case Field_slot:
		declare_slot(s, f);
		set_member(&s->set, f, "p.p", s->ftab.str, strf(&buf, "%s%s", s->var.str, field_name(f)));
		get_member(&s->get, f, "p.p", s->ftab.str, strf(&buf, "%s%s", s->var.str, field_name(f)));
		break;

	case Field_group:
		define_group(s, f->group, field_name(f));
		break;
	}
}

static void define_getter_functions(struct node* node, struct field* field,
                        struct strings* s)
{
        /**
         * define getter
         */
        str_addf(&s->pub_get_header, "\n%s %s_get_%s(%s_ptr p);\n", field->v.tname, node->name.str,
                 field_name(field), node->name.str);
        str_addf(&s->pub_get, "\n%s %s_get_%s(%s_ptr p)\n", field->v.tname, node->name.str,
                 field_name(field), node->name.str);
        struct str getter_body = STR_INIT;
        get_member(&getter_body, field, "p.p", "", field_name(field));
		str_addf(&s->pub_get, "{\n");
        str_addf(&s->pub_get, "%s%s %s;\n", s->ftab.str, field->v.tname, field_name(field));
        str_addf(&s->pub_get, "%s%s", s->ftab.str,
                 getter_body.str);
        str_release(&getter_body);
        str_addf(&s->pub_get, "%sreturn %s;\n}\n", s->ftab.str,
                 field_name(field));
}

static void define_setter_functions(struct node* node, struct field* field,
                        struct strings* s)
{
        str_addf(&s->pub_set_header, "\nvoid %s_set_%s(%s_ptr p, %s %s);\n",node->name.str,
                 field_name(field), node->name.str, field->v.tname,
                 field_name(field));
        str_addf(&s->pub_set, "\nvoid %s_set_%s(%s_ptr p, %s %s)\n",node->name.str,
                 field_name(field), node->name.str, field->v.tname,
                 field_name(field));
        struct str setter_body = STR_INIT;
        set_member(&setter_body, field, "p.p", s->ftab.str, field_name(field));
        str_addf(&s->pub_set, "{\n%s}\n", setter_body.str);
        str_release(&setter_body);
}

static void define_group(struct strings *s, struct node *n, const char *group_name) {
	struct field *f;
	int flen = capn_len(n->n._struct.fields);
	int ulen = n->n._struct.discriminantCount;
	/* named union is where all group members are in the union */
	int named_union = (group_name && ulen == flen && ulen > 0);
	int named_struct = (group_name && !named_union);
	int empty = 1;

	for (f = n->fields; f < n->fields + flen; f++) {
		decode_value(&f->v, f->f.slot.type, f->f.slot.defaultValue, NULL);
		if (f->v.t.which != Type__void)
			empty = 0;
	}

	if (named_struct && empty) {
		str_addf(&s->decl, "%s/* struct { -empty- } %s; */\n", s->dtab.str, group_name);
		return;
	}

	if (named_struct) {
		str_addf(&s->decl, "%scapnp_nowarn struct {\n", s->dtab.str);
		str_add(&s->dtab, "\t", 1);
	}

	if (group_name) {
		str_addf(&s->var, "%s.", group_name);
	}

	/* fields before the union members */
	for (f = n->fields; f < n->fields + flen && !in_union(f); f++) {
		define_field(s, f);

		if (!g_fieldgetset) {
			continue;
		}

		if ((n->n.which == Node__struct && n->n._struct.isGroup)) {
			// Don't emit in-place getters and setters for groups because they
			// are defined as anonymous structs inside their parent struct.
			// We could do it, but nested structs shouldn't be accessed
			// in-place anyway.
			continue;
		}

		if (f->v.t.which == Type__void) {
			continue;
		}

		define_getter_functions(n, f, s);
		define_setter_functions(n, f, s);
	}

	if (ulen > 0) {
		do_union(s, n, f, named_union ? group_name : NULL);

		while (f < n->fields + flen && in_union(f))
			f++;

		/* fields after the unnamed union */
		for (;f < n->fields + flen; f++) {
			define_field(s, f);
		}
	}

	if (named_struct) {
		str_setlen(&s->dtab, s->dtab.len-1);
		str_addf(&s->decl, "%s} %s;\n", s->dtab.str, group_name);
	}

	if (group_name) {
		str_setlen(&s->var, s->var.len - strlen(group_name) - 1);
	}
}

static void define_struct(struct node *n) {
	static struct strings s;

	str_reset(&s.dtab);
	str_reset(&s.ftab);
	str_reset(&s.get);
	str_reset(&s.set);
	str_reset(&s.enums);
	str_reset(&s.decl);
	str_reset(&s.var);
	str_reset(&s.pub_get);
	str_reset(&s.pub_set);
	str_reset(&s.pub_get_header);
	str_reset(&s.pub_set_header);

	str_add(&s.dtab, "\t", -1);
	str_add(&s.ftab, "\t", -1);
	str_add(&s.var, "s->", -1);

	define_group(&s, n, NULL);

	str_add(&HDR, s.enums.str, s.enums.len);

	str_addf(&HDR, "\n%sstruct %s {\n",
			s.decl.len == 0 ? "capnp_nowarn " : "",
			n->name.str);
	str_add(&HDR, s.decl.str, s.decl.len);
	str_addf(&HDR, "};\n");

	str_addf(&SRC, "\n%s_ptr new_%s(struct capn_segment *s) {\n", n->name.str, n->name.str);
	str_addf(&SRC, "\t%s_ptr p;\n", n->name.str);
	str_addf(&SRC, "\tp.p = capn_new_struct(s, %d, %d);\n", 8*n->n._struct.dataWordCount, n->n._struct.pointerCount);
	str_addf(&SRC, "\treturn p;\n");
	str_addf(&SRC, "}\n");

	// adding the ability to get the structure size
	str_addf(&HDR, "\nstatic const size_t %s_word_count = %d;\n", n->name.str, n->n._struct.dataWordCount);
	str_addf(&HDR, "\nstatic const size_t %s_pointer_count = %d;\n", n->name.str, n->n._struct.pointerCount);
	str_addf(&HDR, "\nstatic const size_t %s_struct_bytes_count = %d;\n", n->name.str, 8 * (n->n._struct.pointerCount + n->n._struct.dataWordCount));

	str_addf(&SRC, "%s_list new_%s_list(struct capn_segment *s, int len) {\n", n->name.str, n->name.str);
	str_addf(&SRC, "\t%s_list p;\n", n->name.str);
	str_addf(&SRC, "\tp.p = capn_new_list(s, len, %d, %d);\n", 8*n->n._struct.dataWordCount, n->n._struct.pointerCount);
	str_addf(&SRC, "\treturn p;\n");
	str_addf(&SRC, "}\n");

	str_addf(&SRC, "void read_%s(struct %s *s capnp_unused, %s_ptr p) {\n", n->name.str, n->name.str, n->name.str);
	str_addf(&SRC, "\tcapn_resolve(&p.p);\n\tcapnp_use(s);\n");
	str_add(&SRC, s.get.str, s.get.len);
	str_addf(&SRC, "}\n");

	str_addf(&SRC, "void write_%s(const struct %s *s capnp_unused, %s_ptr p) {\n", n->name.str, n->name.str, n->name.str);
	str_addf(&SRC, "\tcapn_resolve(&p.p);\n\tcapnp_use(s);\n");
	str_add(&SRC, s.set.str, s.set.len);
	str_addf(&SRC, "}\n");

	str_addf(&SRC, "void get_%s(struct %s *s, %s_list l, int i) {\n", n->name.str, n->name.str, n->name.str);
	str_addf(&SRC, "\t%s_ptr p;\n", n->name.str);
	str_addf(&SRC, "\tp.p = capn_getp(l.p, i, 0);\n");
	str_addf(&SRC, "\tread_%s(s, p);\n", n->name.str);
	str_addf(&SRC, "}\n");

	str_addf(&SRC, "void set_%s(const struct %s *s, %s_list l, int i) {\n", n->name.str, n->name.str, n->name.str);
	str_addf(&SRC, "\t%s_ptr p;\n", n->name.str);
	str_addf(&SRC, "\tp.p = capn_getp(l.p, i, 0);\n");
	str_addf(&SRC, "\twrite_%s(s, p);\n", n->name.str);
	str_addf(&SRC, "}\n");

	str_add(&SRC, s.pub_get.str, s.pub_get.len);
	str_add(&SRC, s.pub_set.str, s.pub_set.len);

	str_add(&HDR, s.pub_get_header.str, s.pub_get_header.len);
	str_add(&HDR, s.pub_set_header.str, s.pub_set_header.len);
}

#if 0
/* Commenting out interfaces until the RPC protocol has been spec'd */
static int find_offset(struct str *v, int inc, uint64_t mask) {
	int i, j;
	union {uint64_t u; char c[8];} umask;
	umask.u = capn_flip64(mask);
	for (i = 0; i < v->len*8; i += inc) {
		for (j = i; j < i+inc; j++) {
			if (((uint8_t*)v->str)[j/8] & (1 << (j%8))) {
				goto loop;
			}
		}

		for (j = i; j < i+inc; j++) {
			((uint8_t*)v->str)[j/8] |= 1 << (j%8);
		}

		return j/inc;
loop:
		continue;
	}
	str_add(v, umask.c, 8);
	return (v->len-8)/inc;
}

static void define_method(struct node *iface, int ord) {
	static struct str buf = STR_INIT;
	struct member *mbrs;
	struct InterfaceNode_Method method;
	int i, ptrs = 0, datasz;

	get_InterfaceNode_Method(&method, iface->i.methods, ord);
	mbrs = calloc(method.params.p.len, sizeof(*mbrs));

	str_reset(&buf);

	for (i = 0; i < method.params.p.len; i++) {
		struct InterfaceNode_Method_Param param;
		struct member *m = &mbrs[i];

		get_InterfaceNode_Method_Param(&param, method.params, i);
		decode_value(&m->v, param.type, param.defaultValue, NULL);
		m->m.name = param.name;
		m->m.annotations = param.annotations;
		m->m.ordinal = i;

		switch (m->v.t.body_tag) {
		case Type__void:
			break;
		case Type__bool:
			m->f.offset = find_offset(&buf, 1, 1);
			break;
		case Type_int8:
		case Type_uint8:
			m->f.offset = find_offset(&buf, 8, 0xFF);
			break;
		case Type_int16:
		case Type_uint16:
		case Type__enum:
			m->f.offset = find_offset(&buf, 16, 0xFFFF);
			break;
		case Type_int32:
		case Type_uint32:
		case Type_float32:
			m->f.offset = find_offset(&buf, 32, 0xFFFFFFFFu);
			break;
		case Type_int64:
		case Type_uint64:
		case Type_float64:
			m->f.offset = find_offset(&buf, 64, ~((uint64_t) 0));
			break;
		case Type_text:
		case Type_data:
		case Type__list:
		case Type__struct:
		case Type__interface:
		case Type_anyPointer:
			m->f.offset = ptrs++;
			break;
		}
	}

	datasz = buf.len;


	/* write function to initiate a call */

	str_addf(&HDR, "\nint write_%s_%s(struct capn_msg*", iface->name.str, method.name.str);
	str_addf(&SRC, "\nint write_%s_%s(struct capn_msg *m", iface->name.str, method.name.str);
	for (i = 0; i < method.params.p.len; i++) {
		struct member *m = &mbrs[i];
		str_addf(&HDR, ", %s %s", m->v.tname, m->m.name.str);
		str_addf(&SRC, ", %s a%d", m->v.tname, i);
	}
	str_addf(&HDR, ");\n");
	str_addf(&SRC, ") {\n");

	str_addf(&SRC, "\tint err = 0;\n");
	str_addf(&SRC, "\tm->method = %d;\n", ord);
	str_addf(&SRC, "\tm->iface = ((uint64_t) %#xu << 32) | %#xu;\n", (uint32_t) (iface->n.id >> 32), (uint32_t) iface->n.id);

	if (datasz || ptrs) {
		str_addf(&SRC, "\tm->args = capn_new_struct(m->seg, %d, %d);\n", datasz, ptrs);
	} else {
		g_nullused = 1;
		str_addf(&SRC, "\tm->args = capn_null;\n");
	}

	for (i = 0; i < method.params.p.len; i++) {
		set_member(&mbrs[i], "m->args", "\t", strf(&buf, "a%d", i));
	}

	str_addf(&SRC, "\treturn err;\n");
	str_addf(&SRC, "}\n");


	/* read function to handle a call */

	if (datasz || ptrs) {
		str_addf(&HDR, "void read_%s_%s(struct capn_msg*", iface->name.str, method.name.str);
		str_addf(&SRC, "void read_%s_%s(struct capn_msg *m", iface->name.str, method.name.str);
		for (i = 0; i < method.params.p.len; i++) {
			struct member *m = &mbrs[i];
			str_addf(&HDR, ", %s *%s", m->v.tname, m->m.name.str);
			str_addf(&SRC, ", %s *a%d", m->v.tname, i);
		}
		str_addf(&HDR, ");\n");
		str_addf(&SRC, ") {\n");

		for (i = 0; i < method.params.p.len; i++) {
			get_member(&mbrs[i], "m->args", "\t", strf(&buf, "*a%d", i));
		}

		str_addf(&SRC, "}\n");
	}

	free(mbrs);
}
#endif

static void declare(struct node *file_node, const char *format, int num) {
	struct node *n;
	str_addf(&HDR, "\n");
	for (n = file_node->file_nodes; n != NULL; n = n->next_file_node) {
		if (n->n.which == Node__struct && !n->n._struct.isGroup) {
			switch (num) {
			case 3:
				str_addf(&HDR, format, n->name.str, n->name.str, n->name.str);
				break;
			case 2:
				str_addf(&HDR, format, n->name.str, n->name.str);
				break;
			case 1:
				str_addf(&HDR, format, n->name.str);
				break;
			}
		}
	}
}

int main() {
	struct capn capn;
	CodeGeneratorRequest_ptr root;
	struct CodeGeneratorRequest req;
	struct node *file_node, *n;
	struct node *all_files = NULL, *all_structs = NULL;
	int i, j;
	uint64_t total_len=0;
	struct capn_segment *current_seg;

	if (capn_init_fp(&capn, stdin, 0)==-1) {
		fprintf(stderr, "failed to read schema from stdin\n");
		return -1;
	}

	current_seg = capn.seglist;
	do{
		total_len+=current_seg->len;
		current_seg = current_seg->next;
	}while(current_seg);

	g_valseg.data = calloc(1, total_len);
	g_valseg.cap = total_len;

	root.p = capn_getp(capn_root(&capn), 0, 1);
	read_CodeGeneratorRequest(&req, root);

	for (i = 0; i < capn_len(req.nodes); i++) {
		n = calloc(1, sizeof(*n));
		get_Node(&n->n, req.nodes, i);
		insert_node(n);

		switch (n->n.which) {
		case Node_file:
			n->next = all_files;
			all_files = n;
			break;

		case Node__struct:
			n->next = all_structs;
			all_structs = n;
			break;

		default:
			break;
		}
	}

	for (n = all_structs; n != NULL; n = n->next) {
		n->fields = calloc(capn_len(n->n._struct.fields), sizeof(n->fields[0]));
		for (j = 0; j < capn_len(n->n._struct.fields); j++) {
			decode_field(n->fields, n->n._struct.fields, j);
		}
	}

	for (n = all_files; n != NULL; n = n->next) {
		struct str b = STR_INIT;

		for (i = capn_len(n->n.nestedNodes)-1; i >= 0; i--) {
			struct Node_NestedNode nest;
			get_Node_NestedNode(&nest, n->n.nestedNodes, i);
			struct node *nn = find_node_mayfail(nest.id);
			if (nn) {
				resolve_names(&b, nn, nest.name, n);
			}
		}

		str_release(&b);
	}

	for (i = 0; i < capn_len(req.requestedFiles); i++) {
		struct CodeGeneratorRequest_RequestedFile file_req;
		static struct str b = STR_INIT;
		char *p;
		const char *nameinfix = NULL;
		FILE *srcf, *hdrf;

		g_valc = 0;
		g_valseg.len = 0;
		g_val0used = 0;
		g_nullused = 0;
		capn_init_malloc(&g_valcapn);
		capn_append_segment(&g_valcapn, &g_valseg);

		get_CodeGeneratorRequest_RequestedFile(&file_req, req.requestedFiles, i);
		file_node = find_node(file_req.id);
		if (file_node == NULL) {
			fprintf(stderr, "invalid file_node specified\n");
			exit(2);
		}

		for (j = capn_len(file_node->n.annotations)-1; j >= 0; j--) {
			struct Annotation a;
			struct Value v;
			get_Annotation(&a, file_node->n.annotations, j);
			read_Value(&v, a.value);

			switch (a.id) {
			case 0x85a8d86d736ba637UL:	/* $C::nameinfix */
				if (v.which != Value_text) {
					fprintf(stderr, "schema breakage on $C::nameinfix annotation\n");
					exit(2);
				}
				if (nameinfix) {
					fprintf(stderr, "$C::nameinfix annotation appears more than once\n");
					exit(2);
				}
				nameinfix = v.text.str ? v.text.str : "";
				break;
			case 0xf72bc690355d66deUL:	/* $C::fieldgetset */
				g_fieldgetset = 1;
				break;
			}
		}
		if (!nameinfix)
			nameinfix = "";

		str_reset(&HDR);
		str_reset(&SRC);

		str_addf(&HDR, "#ifndef CAPN_%X%X\n", (uint32_t) (file_node->n.id >> 32), (uint32_t) file_node->n.id);
		str_addf(&HDR, "#define CAPN_%X%X\n", (uint32_t) (file_node->n.id >> 32), (uint32_t) file_node->n.id);
		str_addf(&HDR, "/* AUTO GENERATED - DO NOT EDIT */\n");
		str_addf(&HDR, "#include <capnp_c.h>\n\n");
		str_addf(&HDR, "#if CAPN_VERSION != 1\n");
		str_addf(&HDR, "#error \"version mismatch between capnp_c.h and generated code\"\n");
		str_addf(&HDR, "#endif\n\n");
		str_addf(&HDR, "#ifndef capnp_nowarn\n"
				"# ifdef __GNUC__\n"
				"#  define capnp_nowarn __extension__\n"
				"# else\n"
				"#  define capnp_nowarn\n"
				"# endif\n"
				"#endif\n\n");

		for (j = 0; j < capn_len(file_req.imports); j++) {
			struct CodeGeneratorRequest_RequestedFile_Import im;
			get_CodeGeneratorRequest_RequestedFile_Import(&im, file_req.imports, j);

			// Ignore leading slashes when generating C file #include's.
			// This signifies an absolute import in a library directory.
			const char *base_path = im.name.str[0] == '/' ? &im.name.str[1] : im.name.str;
			str_addf(&HDR, "#include \"%s%s.h\"\n", base_path, nameinfix);
		}

		str_addf(&HDR, "\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n");

		declare(file_node, "struct %s;\n", 1);
		declare(file_node, "typedef struct {capn_ptr p;} %s_ptr;\n", 1);
		declare(file_node, "typedef struct {capn_ptr p;} %s_list;\n", 1);

		for (n = file_node->file_nodes; n != NULL; n = n->next_file_node) {
			if (n->n.which == Node__enum) {
				define_enum(n);
			}
		}

		for (n = file_node->file_nodes; n != NULL; n = n->next_file_node) {
			if (n->n.which == Node__const) {
				define_const(n);
			}
		}

		for (n = file_node->file_nodes; n != NULL; n = n->next_file_node) {
			if (n->n.which == Node__struct && !n->n._struct.isGroup) {
				define_struct(n);
			}
		}

		declare(file_node, "%s_ptr new_%s(struct capn_segment*);\n", 2);
		declare(file_node, "%s_list new_%s_list(struct capn_segment*, int len);\n", 2);
		declare(file_node, "void read_%s(struct %s*, %s_ptr);\n", 3);
		declare(file_node, "void write_%s(const struct %s*, %s_ptr);\n", 3);
		declare(file_node, "void get_%s(struct %s*, %s_list, int i);\n", 3);
		declare(file_node, "void set_%s(const struct %s*, %s_list, int i);\n", 3);

		str_addf(&HDR, "\n#ifdef __cplusplus\n}\n#endif\n#endif\n");

		/* write out the header */

		hdrf = fopen(strf(&b, "%s%s.h", file_node->n.displayName.str, nameinfix), "w");
		if (!hdrf) {
			fprintf(stderr, "failed to open %s: %s\n", b.str, strerror(errno));
			exit(2);
		}
		fwrite(HDR.str, 1, HDR.len, hdrf);
		fclose(hdrf);

		/* write out the source */

		srcf = fopen(strf(&b, "%s%s.c", file_node->n.displayName.str, nameinfix), "w");
		if (!srcf) {
			fprintf(stderr, "failed to open %s: %s\n", b.str, strerror(errno));
			exit(2);
		}
		p = strrchr(file_node->n.displayName.str, '/');
		fprintf(srcf, "#include \"%s%s.h\"\n", p ? p+1 : file_node->n.displayName.str, nameinfix);
		fprintf(srcf, "/* AUTO GENERATED - DO NOT EDIT */\n");
		fprintf(srcf, "#ifdef __GNUC__\n"
				"# define capnp_unused __attribute__((unused))\n"
				"# define capnp_use(x) (void) x;\n"
				"#else\n"
				"# define capnp_unused\n"
				"# define capnp_use(x)\n"
				"#endif\n\n");


		if (g_val0used)
			fprintf(srcf, "static const capn_text capn_val0 = {0,\"\",0};\n");
		if (g_nullused)
			fprintf(srcf, "static const capn_ptr capn_null = {CAPN_NULL};\n");

		if (g_valseg.len > 8) {
			size_t k;
			fprintf(srcf, "static const uint8_t capn_buf[%zu] = {", g_valseg.len-8);
			for (k = 8; k < g_valseg.len; k++) {
				if (k > 8)
					fprintf(srcf, ",");
				if ((k % 8) == 0)
					fprintf(srcf, "\n\t");
				fprintf(srcf, "%u", ((uint8_t*)g_valseg.data)[k]);
			}
			fprintf(srcf, "\n};\n");

			fprintf(srcf, "static const struct capn_segment capn_seg = {{0},0,0,0,(char*)&capn_buf[0],%zu,%zu,0};\n",
					g_valseg.len-8, g_valseg.len-8);
		}

		fwrite(SRC.str, 1, SRC.len, srcf);
		fclose(srcf);

		capn_free(&g_valcapn);
	}

	return 0;
}
