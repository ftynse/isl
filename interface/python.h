#include "generator.h"

using namespace std;
using namespace clang;

void generate_python(set<RecordDecl *> &exported_types,
	set<FunctionDecl *> exported_functions, set<FunctionDecl *> functions);
