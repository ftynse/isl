#include "generator.h"

using namespace std;
using namespace clang;

class cpp_generator : public generator {
public:
	cpp_generator(set<RecordDecl *> &exported_types,
		set<FunctionDecl *> exported_functions,
		set<FunctionDecl *> functions);

	virtual void generate() override;
private:

	/* Print forward declarations for all classes to "os".
	 */
	void print_forward_declarations(ostream &os);

	/* Print all declarations to "os".
	 */
	void print_declarations(ostream &os);

	/* Print declarations for class "clazz" to "os".
	 */
	void print_class(ostream &os, const isl_class &clazz);

	/* Print forward declaration of class "clazz" to "os".
	 */
	void print_class_forward_decl(ostream &os, const isl_class &clazz);

	/* Print global constructor method to "os".
	 *
	 * Each class has one global constructor:
	 *
	 * 	Set manage(__isl_take isl_set *Ptr);
	 *
	 * The only public way to construct isl C++ objects from a raw pointer
	 * is through this global constructor method. This ensures isl object
	 * constructions is very explicit and pointers do not converted by
	 * accident. Due to overloading, manage() can be called on any isl
	 * raw pointer and the corresponding object is automatically
	 * constructed, without the user having to choose the right isl object
	 * type.
	 *
	 */
	void print_class_global_constructor(ostream &os,
		const isl_class &clazz);

	/* Print declarations of private constructors for class "clazz" to "os".
	 *
	 * Each class currently as one private constructor:
	 *
	 * 	1) Constructor from a plain isl_* C pointer
	 *
	 * Example:
	 *
	 * 	Set(__isl_take isl_set *Ptr);
	 *
	 * The raw pointer constructor is kept private. Object construction
	 * is only possible through isl::manage().
	 */
	void print_private_constructors(ostream &os, const isl_class &clazz);

	/* Print declarations of copy assignment operator for class "clazz"
	 * to "os".
	 *
	 * Each class has one assignment operator.
	 *
	 * 	Set& Set::operator=(Set Obj)
	 *
	 */
	void print_copy_assignment(ostream &os, const isl_class &clazz);

	/* Print declarations of public constructors for class "clazz" to "os".
	 *
	 * Each class currently as two public constructors:
	 *
	 * 	1) A default constructor
	 * 	2) A copy constructor
	 *
	 * Example:
	 *
	 *	Set();
	 *	Set(const Set &set);
	 */
	void print_public_constructors(ostream &os, const isl_class &clazz);

	/* Print declaration of destructor for class "clazz" to "os".
	 */
	void print_destructor(ostream &os, const isl_class &clazz);

	/* Print declaration of pointer functions for class "clazz" to "os".
	 *
	 * To obtain a raw pointer three functions are provided:
	 *
	 * 	1) __isl_give isl_set *copy()
	 *
	 * 	  Returns a pointer to a _copy_ of the internal object
	 *
	 * 	2) __isl_keep isl_set *get()
	 *
	 * 	  Returns a pointer to the internal object
	 *
	 * 	3) __isl_give isl_set *release()
	 *
	 * 	  Returns a pointer to the internal object and resets the
	 * 	  internal pointer to nullptr.
	 *
	 * 	4) bool isNull()
	 *
	 * 	  Check if the current object is a null pointer.
	 *
	 * The functions get() and release() model std::unique ptr. The copy()
	 * function is an extension to allow the user to explicitly copy
	 * the underlying object.
	 *
	 * Also generate a declaration to delete copy() for r-values. For
	 * r-values release() should be used to avoid unnecessary copies.
	 */
	void print_ptr(ostream &os, const isl_class &clazz);

	/* Print all implementations to "os".
	 */
	void print_implementations(ostream &os);

	/* Print implementations for class "clazz" to "os".
	 */
	void print_class_impl(ostream &os, const isl_class &clazz);

	/* Print implementation of global constructor method to "os".
	 */
	void print_class_global_constructor_impl(ostream &os,
		const isl_class &clazz);

	/* Print implementations of private constructors for class "clazz" to
	 * "os".
	 */
	void print_private_constructors_impl(ostream &os,
		const isl_class &clazz);

	/* Print implementations of public constructors for class "clazz" to
	 * "os".
	 */
	void print_public_constructors_impl(ostream &os,
		const isl_class &clazz);

	/* Print implementation of copy assignment operator for class "clazz"
	 * to "os".
	 */
	void print_copy_assignment_impl(ostream &os, const isl_class &clazz);

	/* Print implementation of destructor for class "clazz" to "os".
	 */
	void print_destructor_impl(ostream &os, const isl_class &clazz);

	/* Print implementation of ptr() function for class "clazz" to "os".
	 */
	void print_ptr_impl(ostream &os, const isl_class &clazz);

	/* Translate the isl type name into its corresponding C++ type.
	 *
	 * To obtain the C++ type name, the 'isl_' prefix is removed and the
	 * remainder * is translated to CamelCase.
	 *
	 * isl_basic_set -> BasicSet
	 *
	 */
	string type2cpp(string name);

	/* Convert a string "input" to CamelCase.
	 */
	string to_camel_case(const string &input);

	/* Check if a given class "clazz" is supported by these bindings.
	 *
	 * Only classes that have been tested and reviewed are supported.
	 */
	bool is_supported_class(string name);
};
