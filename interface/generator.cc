/*
 * Copyright 2011,2015 Sven Verdoolaege. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY SVEN VERDOOLAEGE ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SVEN VERDOOLAEGE OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as
 * representing official policies, either expressed or implied, of
 * Sven Verdoolaege.
 */

#include <clang/AST/Attr.h>

#include "isl_config.h"
#include "extract_interface.h"

#include "generator.h"

#include <iostream>

/* Should "method" be considered to be a static method?
 * That is, is the first argument something other than
 * an instance of the class?
 */
bool generator::is_static(const isl_class &clazz, FunctionDecl *method)
{
	ParmVarDecl *param = method->getParamDecl(0);
	QualType type = param->getOriginalType();

	if (!is_isl_type(type))
		return true;
	return extract_type(type) != clazz.name;
}

/*  Collect all functions that belong to a certain type, separating
 *  constructors from regular methods and keeping track of the _to_str and
 *  _free functions, if any, separately.  If there are any overloaded
 *  functions, then they are grouped based on their name after removing the
 *  argument type suffix.
 */
generator::generator(set<RecordDecl *> &exported_types,
	set<FunctionDecl *> exported_functions, set<FunctionDecl *> functions)
{
	map<string, isl_class>::iterator ci;

	set<FunctionDecl *>::iterator in;
	for (in = functions.begin(); in != functions.end(); ++in) {
		FunctionDecl *decl = *in;
		functions_by_name[decl->getName()] = decl;
	}

	set<RecordDecl *>::iterator it;
	for (it = exported_types.begin(); it != exported_types.end(); ++it) {
		RecordDecl *decl = *it;
		map<string, FunctionDecl *>::iterator i;

		string name = decl->getName();
		classes[name].name = name;
		classes[name].type = decl;
		classes[name].fn_to_str = NULL;
		classes[name].fn_free = NULL;

		i = functions_by_name.find(name + "_to_str");
		if (i != functions_by_name.end())
			classes[name].fn_to_str = i->second;

		i = functions_by_name.find (name + "_free");
		if (i == functions_by_name.end())
			die("No _free function found");
		classes[name].fn_free = i->second;
	}

	for (in = exported_functions.begin(); in != exported_functions.end();
	     ++in) {
		isl_class *c = method2class(classes, *in);
		if (!c)
			continue;
		if (is_constructor(*in)) {
			c->constructors.insert(*in);
		} else {
			FunctionDecl *method = *in;
			string fullname = method->getName();
			fullname = drop_type_suffix(fullname, method);
			c->methods[fullname].insert(method);
		}
	}
}

/* Print error message "msg" and abort.
 */
void generator::die(const char *msg)
{
	fprintf(stderr, "%s", msg);
	abort();
}

/* If "method" is overloaded, then drop the suffix of "name"
 * corresponding to the type of the final argument and
 * return the modified name (or the original name if
 * no modifications were made).
 */
string generator::drop_type_suffix(string name, FunctionDecl *method)
{
	int num_params;
	ParmVarDecl *param;
	string type;
	size_t name_len, type_len;

	if (!is_overload(method))
		return name;

	num_params = method->getNumParams();
	param = method->getParamDecl(num_params - 1);
	type = extract_type(param->getOriginalType());
	type = type.substr(4);
	name_len = name.length();
	type_len = type.length();

	if (name_len > type_len && name.substr(name_len - type_len) == type)
		name = name.substr(0, name_len - type_len - 1);

	return name;
}

/* Return a sequence of the types of which the given type declaration is
 * marked as being a subtype.
 */
std::vector<string> generator::find_superclasses(RecordDecl *decl)
{
	vector<string> super;

	if (!decl->hasAttrs())
		return super;

	string sub = "isl_subclass";
	size_t len = sub.length();
	AttrVec attrs = decl->getAttrs();
	for (AttrVec::const_iterator i = attrs.begin() ; i != attrs.end(); ++i) {
		const AnnotateAttr *ann = dyn_cast<AnnotateAttr>(*i);
		if (!ann)
			continue;
		string s = ann->getAnnotation().str();
		if (s.substr(0, len) == sub) {
			s = s.substr(len + 1, s.length() - len  - 2);
			super.push_back(s);
		}
	}

	return super;
}

/* Is decl marked as being part of an overloaded method?
 */
bool generator::is_overload(Decl *decl)
{
	return has_annotation(decl, "isl_overload");
}

/* Is decl marked as a constructor?
 */
bool generator::is_constructor(Decl *decl)
{
	return has_annotation(decl, "isl_constructor");
}

/* Is decl marked as consuming a reference?
 */
bool generator::takes(Decl *decl)
{
	return has_annotation(decl, "isl_take");
}

/* Is decl marked as returning a reference that is required to be freed.
 */
bool generator::gives(Decl *decl)
{
	return has_annotation(decl, "isl_give");
}

/* Return the class that has a name that matches the initial part
 * of the name of function "fd" or NULL if no such class could be found.
 */
isl_class *generator::method2class(map<string, isl_class> &classes,
	FunctionDecl *fd)
{
	string best;
	map<string, isl_class>::iterator ci;
	string name = fd->getNameAsString();

	for (ci = classes.begin(); ci != classes.end(); ++ci) {
		if (name.substr(0, ci->first.length()) == ci->first)
			best = ci->first;
	}

	if (classes.find(best) == classes.end()) {
		cerr << "Unable to find class of " << name << endl;
		return NULL;
	}

	return &classes[best];
}

/* Is "type" the type "isl_ctx *"?
 */
bool generator::is_isl_ctx(QualType type)
{
	if (!type->isPointerType())
		return 0;
	type = type->getPointeeType();
	if (type.getAsString() != "isl_ctx")
		return false;

	return true;
}

/* Is the first argument of "fd" of type "isl_ctx *"?
 */
bool generator::first_arg_is_isl_ctx(FunctionDecl *fd)
{
	ParmVarDecl *param;

	if (fd->getNumParams() < 1)
		return false;

	param = fd->getParamDecl(0);
	return is_isl_ctx(param->getOriginalType());
}

/* Is "type" that of a pointer to an isl_* structure?
 */
bool generator::is_isl_type(QualType type)
{
	if (type->isPointerType()) {
		string s;

		type = type->getPointeeType();
		if (type->isFunctionType())
			return false;
		s = type.getAsString();
		return s.substr(0, 4) == "isl_";
	}

	return false;
}

/* Is "type" the type isl_bool?
 */
bool generator::is_isl_bool(QualType type)
{
	string s;

	if (type->isPointerType())
		return false;

	s = type.getAsString();
	return s == "isl_bool";
}

/* Is "type" that of a pointer to char.
 */
bool generator::is_string_type(QualType type)
{
	if (type->isPointerType()) {
		string s;

		type = type->getPointeeType();
		if (type->isFunctionType())
			return false;
		s = type.getAsString();
		return s == "const char" || "char";
	}

	return false;
}

/* Is "type" that of a pointer to a function?
 */
bool generator::is_callback(QualType type)
{
	if (!type->isPointerType())
		return false;
	type = type->getPointeeType();
	return type->isFunctionType();
}

/* Is "type" that of "char *" of "const char *"?
 */
bool generator::is_string(QualType type)
{
	if (type->isPointerType()) {
		string s = type->getPointeeType().getAsString();
		return s == "const char" || s == "char";
	}

	return false;
}

/* Return the name of the type that "type" points to.
 * The input "type" is assumed to be a pointer type.
 */
string generator::extract_type(QualType type)
{
	if (type->isPointerType())
		return type->getPointeeType().getAsString();
	die("Cannot extract type from non-pointer type");
}
