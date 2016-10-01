#ifndef ISL_INTERFACE_GENERATOR_H
#define ISL_INTERFACE_GENERATOR_H

#include <clang/AST/Decl.h>
#include <map>
#include <set>
#include <string>


using namespace std;
using namespace clang;

/* isl_class collects all constructors and methods for an isl "class".
 * "name" is the name of the class.
 * "type" is the declaration that introduces the type.
 * "methods" contains the set of methods, grouped by method name.
 * "fn_to_str" is a reference to the *_to_str method of this class, if any.
 * "fn_free" is a reference to the *_free method of this class, if any.
 */
struct isl_class {
	string name;
	RecordDecl *type;
	set<FunctionDecl *> constructors;
	map<string, set<FunctionDecl *> > methods;
	FunctionDecl *fn_to_str;
	FunctionDecl *fn_free;

	bool is_static(FunctionDecl *method);

	void print(map<string, isl_class> &classes, set<string> &done);
	void print_constructor(FunctionDecl *method);
	void print_representation(const string &python_name);
	void print_method_type(FunctionDecl *fd);
	void print_method_types();
	void print_method(FunctionDecl *method, vector<string> super);
	void print_method_overload(FunctionDecl *method, vector<string> super);
	void print_method(const string &fullname,
		const set<FunctionDecl *> &methods, vector<string> super);
};

void die(const char *msg) __attribute__((noreturn));
string drop_type_suffix(string name, FunctionDecl *method);
vector<string> find_superclasses(RecordDecl *decl);
bool is_overload(Decl *decl);
bool is_constructor(Decl *decl);
bool takes(Decl *decl);
bool gives(Decl *decl);
isl_class *method2class(map<string, isl_class> &classes, FunctionDecl *fd);
bool is_isl_ctx(QualType type);
bool first_arg_is_isl_ctx(FunctionDecl *fd);
bool is_isl_type(QualType type);
bool is_isl_bool(QualType type);
bool is_string_type(QualType type);
bool is_callback(QualType type);
bool is_string(QualType type);
string extract_type(QualType type);

#endif /* ISL_INTERFACE_GENERATOR_H */
