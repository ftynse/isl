/*
 * Copyright 2016 Tobias Grosser. All rights reserved.
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

#include "isl_config.h"

#include <stdio.h>
#include <iostream>
#include <map>
#include <vector>
#include "cpp.h"

/* Print string formatted according to "fmt" to ostream "os".
 *
 * This fprintf method allows us to use printf style formatting constructs when
 * writing to an ostream.
 */
void fprintf(ostream &os, const char *format, ...)
{
	va_list arguments;
	FILE *string_stream;
	char *string_pointer;
	size_t size;

	va_start(arguments, format);

	string_stream = open_memstream(&string_pointer, &size);

	if (!string_stream) {
		fprintf(stderr, "open_memstream failed -- aborting!\n");
		exit(1);
	}

	vfprintf(string_stream, format, arguments);
	fclose(string_stream);
	os << string_pointer;
}

cpp_generator::cpp_generator( set<RecordDecl *> &exported_types,
	set<FunctionDecl *> exported_functions, set<FunctionDecl *> functions)
    : generator(exported_types, exported_functions, functions)
{
}

void cpp_generator::generate()
{
	ostream &os = cout;

	fprintf(os, "\n");
	fprintf(os, "#ifndef ISL_CPP_ALL\n");
	fprintf(os, "#define ISL_CPP_ALL\n\n");
	fprintf(os, "namespace isl {\n\n");

	print_forward_declarations(os);
	print_declarations(os);
	print_implementations(os);

	fprintf(os, "};\n\n");
	fprintf(os, "#endif /* ISL_CPP_ALL */\n");
}

void cpp_generator::print_forward_declarations(ostream &os)
{
	map<string, isl_class>::iterator ci;

	fprintf(os, "// forward declarations\n");

	for (ci = classes.begin(); ci != classes.end(); ++ci)
		if (is_supported_class(ci->second.name))
			print_class_forward_decl(os, ci->second);
	fprintf(os, "\n");
}

void cpp_generator::print_declarations(ostream &os)
{
	map<string, isl_class>::iterator ci;

	for (ci = classes.begin(); ci != classes.end(); ++ci)
		print_class(os, ci->second);
}

void cpp_generator::print_implementations(ostream &os)
{
	map<string, isl_class>::iterator ci;

	for (ci = classes.begin(); ci != classes.end(); ++ci)
		print_class_impl(os, ci->second);
}

void cpp_generator::print_class(ostream &os, const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();

	if (!is_supported_class(clazz.name))
		return;

	fprintf(os, "// declarations for isl::%s\n", cppname);

	print_class_global_constructor(os, clazz);
	fprintf(os, "class %s {\n", cppname);
	fprintf(os, "  friend ");
	print_class_global_constructor(os, clazz);
	fprintf(os, "  %s *Ptr = nullptr;\n", name);
	fprintf(os, "\n");
	print_private_constructors(os, clazz);
	fprintf(os, "\n");
	fprintf(os, "public:\n");
	print_public_constructors(os, clazz);
	print_conversion_constructors(os, clazz);
	print_copy_assignment(os, clazz);
	print_destructor(os, clazz);
	print_ptr(os, clazz);
	print_str(os, clazz);
	print_methods(os, clazz);

	fprintf(os, "};\n\n");
}

void cpp_generator::print_class_forward_decl(ostream &os,
	const isl_class &clazz)
{
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();

	fprintf(os, "class %s;\n", cppname);
}

void cpp_generator::print_class_global_constructor(ostream &os,
	const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();

	fprintf(os, "inline %s manage(__isl_take %s *Ptr);\n\n", cppname,
		name);
}

void cpp_generator::print_private_constructors(ostream &os,
	const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "  inline explicit %s(__isl_take %s *Ptr);\n", cppname,
		name);
}

void cpp_generator::print_public_constructors(ostream &os,
	const isl_class &clazz)
{
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "  inline %s();\n", cppname);
	fprintf(os, "  inline %s(const %s &Obj);\n", cppname, cppname);
}

void cpp_generator::print_conversion_constructors(ostream &os,
       const isl_class &clazz)
{
	set<FunctionDecl *>::const_iterator in;
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();

	for (in = clazz.constructors.begin(); in != clazz.constructors.end();
		++in) {
		auto cons = *in;
		if (!is_supported_constructor(cons))
			continue;
		string fullname = cons->getName();
		ParmVarDecl *param = cons->getParamDecl(0);
		QualType type = param->getOriginalType();
		std::string typestr = type->getPointeeType().getAsString();
		std::string paramstring = type2cpp(typestr);
		const char *paramname = paramstring.c_str();
		fprintf(os, "  inline %s(%s Obj);\n",
		cppname, paramname);
	}
}

void cpp_generator::print_copy_assignment(ostream &os,
	const isl_class &clazz)
{
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "  inline %s& operator=(%s Obj);\n", cppname, cppname);
}

void cpp_generator::print_destructor(ostream &os, const isl_class &clazz)
{
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "  inline ~%s();\n", cppname);
}

void cpp_generator::print_ptr(ostream &os, const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	fprintf(os, "  inline __isl_give %s *copy() const &;\n", name);
	fprintf(os, "  inline __isl_give %s *copy() && = delete;\n", name);
	fprintf(os, "  inline __isl_keep %s *get() const;\n", name);
	fprintf(os, "  inline __isl_give %s *release();\n", name);
	fprintf(os, "  inline bool isNull() const;\n");
	fprintf(os, "  inline operator bool() const;\n");
}

void cpp_generator::print_str(ostream &os, const isl_class &clazz)
{
	if (!clazz.fn_to_str)
		return;

	fprintf(os, "  inline std::string str() const;\n");
}

void cpp_generator::print_methods(ostream &os, const isl_class &clazz)
{
	map<string, set<FunctionDecl *> >::const_iterator it;
	for (it = clazz.methods.begin(); it != clazz.methods.end(); ++it)
		print_method(os, clazz, it->first, it->second);
}

void cpp_generator::print_method(ostream &os, const isl_class &clazz,
	const string &fullname, const set<FunctionDecl *> &methods)
{
	FunctionDecl *any_method;

	any_method = *methods.begin();
	if (methods.size() == 1 && !is_overload(any_method)) {
		print_method(os, clazz, any_method);
		return;
	}

	return;
}

void cpp_generator::print_method(ostream &os, const isl_class &clazz,
	FunctionDecl *method)
{
	if (!is_supported_method(clazz, method))
		return;

	print_method_header(os, clazz, method, true /* is_declaration */);
}

void cpp_generator::print_class_impl(ostream &os, const isl_class &clazz)
{
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();

	if (!is_supported_class(clazz.name))
		return;

	fprintf(os, "// implementations for isl::%s\n", cppname);

	print_class_global_constructor_impl(os, clazz);
	print_public_constructors_impl(os, clazz);
	print_private_constructors_impl(os, clazz);
	print_conversion_constructors_impl(os, clazz);
	print_copy_assignment_impl(os, clazz);
	print_destructor_impl(os, clazz);
	print_ptr_impl(os, clazz);
	print_str_impl(os, clazz);
	print_methods_impl(os, clazz);
}

void cpp_generator::print_class_global_constructor_impl(ostream &os,
	const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();

	fprintf(os, "%s manage(__isl_take %s *Ptr) {\n", cppname, name);
	fprintf(os, "  assert(Ptr && \"NULL ptr cannot be managed\");\n");
	fprintf(os, "  return %s(Ptr);\n", cppname);
	fprintf(os, "}\n\n");
}

void cpp_generator::print_private_constructors_impl(ostream &os,
	const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "%s::%s(__isl_take %s *Ptr) : Ptr(Ptr) {}\n\n", cppname,
		cppname, name);
}

void cpp_generator::print_public_constructors_impl(ostream &os,
	const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "%s::%s() : Ptr(nullptr) {}\n\n", cppname, cppname);
	fprintf(os, "%s::%s(const %s &Obj) : Ptr(Obj.copy()) {}\n\n",
		cppname, cppname, cppname, name);
}

void cpp_generator::print_conversion_constructors_impl(ostream &os,
       const isl_class &clazz)
{
	set<FunctionDecl *>::const_iterator in;
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();

	for (in = clazz.constructors.begin(); in != clazz.constructors.end();
		++in) {
		auto cons = *in;
		if (!is_supported_constructor(cons))
			continue;
		string fullname = cons->getName();
		ParmVarDecl *param = cons->getParamDecl(0);
		QualType type = param->getOriginalType();
		std::string typestr = type->getPointeeType().getAsString();
		std::string paramstring = type2cpp(typestr);
		const char *paramname = paramstring.c_str();
		fprintf(os, "%s::%s(%s Obj) {\n", cppname,
			cppname, paramname);
		fprintf(os, "  Ptr = %s(Obj.release());\n", fullname.c_str());
		fprintf(os, "}\n\n");
	}
}

void cpp_generator::print_copy_assignment_impl(ostream &os,
	const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "%s& %s::operator=(%s Obj) {\n", cppname,
		cppname, cppname);
	fprintf(os, "  std::swap(this->Ptr, Obj.Ptr);\n", name);
	fprintf(os, "  return *this;\n");
	fprintf(os, "}\n\n");
}

void cpp_generator::print_destructor_impl(ostream &os,
	const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "%s::~%s() {\n", cppname, cppname);
	fprintf(os, "  if (Ptr)\n");
	fprintf(os, "    %s_free(Ptr);\n", name);
	fprintf(os, "}\n\n");
}

void cpp_generator::print_ptr_impl(ostream &os, const isl_class &clazz)
{
	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "__isl_give %s *%s::copy() const & {\n", name, cppname);
	fprintf(os, "  assert(Ptr && \"NULL object cannot be copied\");\n");
	fprintf(os, "  return %s_copy(Ptr);\n", name);
	fprintf(os, "}\n\n");
	fprintf(os, "__isl_keep %s *%s::get() const {\n", name, cppname);
	fprintf(os, "  assert(Ptr && \"Cannot return ptr to NULL object\");\n");
	fprintf(os, "  return Ptr;\n");
	fprintf(os, "}\n\n");
	fprintf(os, "__isl_give %s *%s::release() {\n", name, cppname);
	fprintf(os, "  assert(Ptr && \"Cannot return ptr to NULL object\");\n");
	fprintf(os, "  %s *Tmp = Ptr;\n", name);
	fprintf(os, "  Ptr = nullptr;\n");
	fprintf(os, "  return Tmp;\n");
	fprintf(os, "}\n\n");
	fprintf(os, "bool %s::isNull() const {\n", cppname);
	fprintf(os, "  return Ptr == nullptr;\n");
	fprintf(os, "}\n\n");
	fprintf(os, "%s::operator bool() const {\n", cppname);
	fprintf(os, "  return !isNull();\n", name);
	fprintf(os, "}\n\n");
}

void cpp_generator::print_str_impl(ostream &os, const isl_class &clazz)
{
	if (!clazz.fn_to_str)
		return;

	const char *name = clazz.name.c_str();
	std::string cppstring = type2cpp(clazz.name);
	const char *cppname = cppstring.c_str();
	fprintf(os, "std::string %s::str() const {\n", cppname);
	fprintf(os, "  char *Tmp = %s_to_str(get());\n", name, name);
	fprintf(os, "  std::string S(Tmp);\n");
	fprintf(os, "  free(Tmp);\n");
	fprintf(os, "  return Tmp;\n");
	fprintf(os, "}\n\n");
}


void cpp_generator::print_methods_impl(ostream &os, const isl_class &clazz)
{
	map<string, set<FunctionDecl *> >::const_iterator it;
	for (it = clazz.methods.begin(); it != clazz.methods.end(); ++it)
		print_method_impl(os, clazz, it->first, it->second);
}

void cpp_generator::print_method_impl(ostream &os, const isl_class &clazz,
	const string &fullname, const set<FunctionDecl *> &methods)
{
	FunctionDecl *any_method;

	any_method = *methods.begin();
	if (methods.size() == 1 && !is_overload(any_method)) {
		print_method_impl(os, clazz, any_method);
		return;
	}

	return;
}

void cpp_generator::print_method_impl(ostream &os, const isl_class &clazz,
	FunctionDecl *method)
{
	string fullname = method->getName();
	string cname = fullname.substr(clazz.name.length() + 1);
	int num_params = method->getNumParams();

	if (!is_supported_method(clazz, method))
		return;

	print_method_header(os, clazz, method, false /* is_declaration */);

	QualType return_type = method->getReturnType();
	string rettype_str =
		type2cpp(return_type->getPointeeType().getAsString());

	fprintf(os, "   return manage(%s(", fullname.c_str());
	for (int i = 0; i < num_params; ++i) {
		ParmVarDecl *param = method->getParamDecl(i);

		if (i == 0)
			fprintf(os, "");
		else
			fprintf(os, "%s.", param->getName().str().c_str());

		if (takes(param))
			fprintf(os, "copy()");
		else
			fprintf(os, "get()");

		if (i != num_params - 1)
		  fprintf(os, ", ");
	}
	fprintf(os, "));\n");

	fprintf(os, "}\n\n");
}


static const char *cpp_keywords[] = {
  "union",
};

void cpp_generator::print_method_header(ostream &os, const isl_class &clazz,
	FunctionDecl *method, bool is_declaration)
{
	string fullname = method->getName();
	string cname = fullname.substr(clazz.name.length() + 1);
	cname = to_camel_case(cname, true /* start_lowercase */);
	int num_params = method->getNumParams();

	QualType return_type = method->getReturnType();
	string rettype_str =
		type2cpp(return_type->getPointeeType().getAsString());
	string classname = type2cpp(clazz.name);

	if (is_declaration)
		fprintf(os, "  inline %s %s(", rettype_str.c_str(),
			cname.c_str());
	else
		fprintf(os, "%s %s::%s(", rettype_str.c_str(),
			classname.c_str(), cname.c_str());

	for (int i = 1; i < num_params; ++i) {
		ParmVarDecl *param = method->getParamDecl(i);
		QualType type = param->getOriginalType();
		string cpptype = type2cpp(type->getPointeeType().getAsString());
		fprintf(os, "const %s &%s", cpptype.c_str(),
			param->getName().str().c_str());

		if (i != num_params - 1)
		  fprintf(os, ", ");
	}
	if (is_declaration)
		fprintf(os, ") const;\n");
	else
		fprintf(os, ") const {\n");
}

bool cpp_generator::is_supported_method(const isl_class &clazz,
	FunctionDecl *method) {
	string fullname = method->getName();
	string cname = fullname.substr(clazz.name.length() + 1);
	int num_params = method->getNumParams();

	if (first_arg_is_isl_ctx(method))
		return false;

	for (size_t i = 0; i < sizeof(cpp_keywords); i++)
		if (cname.compare(cpp_keywords[i]) == 0)
			return false;

	if (is_static(clazz, method))
		return false;

	for (int i = 1; i < num_params; ++i) {
		ParmVarDecl *param = method->getParamDecl(i);
		QualType type = param->getOriginalType();
		if (!is_isl_type(type))
			return false;
	}

	QualType return_type = method->getReturnType();
	if (!is_isl_type(return_type))
		return false;

	string rettype_str = return_type->getPointeeType().getAsString();

	if (!is_supported_class(rettype_str))
		return false;

	return true;
}

string cpp_generator::to_camel_case(const string &input, bool start_lowercase)
{
	string output;
	bool uppercase = !start_lowercase;

	for (const char &character : input) {
		if (character == '_') {
			uppercase = true;
			continue;
		}
		if (uppercase) {
			output.append(1, toupper(character));
			uppercase = false;
		} else {
			output.append(1, character);
		}
	}

	return output;
}

string cpp_generator::type2cpp(string name)
{
	return to_camel_case(name.substr(4));
}

bool cpp_generator::is_supported_class(string name)
{
	return true;
}

bool cpp_generator::is_supported_constructor(FunctionDecl *cons)
{
	int num_params = cons->getNumParams();
	if (num_params != 1)
		return false;

	ParmVarDecl *param = cons->getParamDecl(0);
	QualType type = param->getOriginalType();
	std::string type_name = type->getPointeeType().getAsString();

	if (type_name == "isl_ctx")
		return false;

	return is_isl_type(type);
}
