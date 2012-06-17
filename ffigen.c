#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <clang-c/Index.h>
#include <assert.h>
#include <jansson.h>

CXTranslationUnit TU;

void putstring(CXString s){
  fprintf(stderr, "%s\n", clang_getCString(s));
  clang_disposeString(s);
}

json_t* render_string_or_null(CXString s) {
  const char* str = clang_getCString(s);
  json_t* js;
  if (str && strlen(str) > 0) js = json_string(str);
  else js = json_null();
  clang_disposeString(s);
  return js;
}

json_t* render_string(CXString s) {
  const char* str = clang_getCString(s);
  json_t* js = json_string(str);
  clang_disposeString(s);
  return js;
}


void json_object_set_with_key_new(json_t* json, json_t* key, json_t* value) {
  assert(json_is_string(key));
  json_object_set_new(json, json_string_value(key), value);
  json_decref(key);
}

json_t* render_type(CXType type) {
  int i;
  CXCursor decl = clang_getTypeDeclaration(type);
  json_t* js = json_object();

  enum CXTypeKind kind = type.kind;
  if (type.kind == CXType_Unexposed) {
    // workaround for libclang bug
    if (clang_getResultType(type).kind != CXType_Invalid) {
      kind = CXType_FunctionProto;
    }
  }


  if (clang_isVolatileQualifiedType(type)) {
    json_object_set_new(js, "volatile", json_true());
  }
  if (clang_isConstQualifiedType(type)) {
    json_object_set_new(js, "const", json_true());
  }
  if (clang_isRestrictQualifiedType(type)) {
    json_object_set_new(js, "restrict", json_true());
  }

  static const struct {
    enum CXTypeKind kind;
    const char* name;
  } PRIMITIVE_TYPES[] = {
    {CXType_Void, "void"},
    {CXType_Bool, "bool"},
    {CXType_Char_U, "char"},
    {CXType_Char_S, "char"},
    {CXType_UChar, "unsigned char"},
    {CXType_SChar, "signed char"},
    {CXType_WChar, "wchar_t"},
    {CXType_Char16, "char16_t"},
    {CXType_Char32, "char32_t"},
    {CXType_Short, "short"},
    {CXType_UShort, "unsigned short"},
    {CXType_Int, "int"},
    {CXType_UInt, "unsigned int"},
    {CXType_Long, "long"},
    {CXType_ULong, "unsigned long"},
    {CXType_LongLong, "unsigned long"},
    {CXType_ULongLong, "unsigned long long"},
    {CXType_Int128, "__int128"},
    {CXType_UInt128, "unsigned __int128"},
    {CXType_Float, "float"},
    {CXType_Double, "double"},
    {CXType_LongDouble, "long double"}
  };


  switch (kind) {
  case CXType_Pointer:
    json_object_set_new(js, "kind", json_string("pointer"));
    json_object_set_new(js, "pointee", render_type(clang_getPointeeType(type)));
    return js;

  case CXType_ConstantArray:
    json_object_set_new(js, "kind", json_string("array"));
    json_object_set_new(js, "element", render_type(clang_getArrayElementType(type)));
    json_object_set_new(js, "length", json_integer(clang_getArraySize(type)));
    return js;
    
  case CXType_FunctionNoProto:
  case CXType_FunctionProto:
    json_object_set_new(js, "kind", json_string("function"));
    json_object_set_new(js, "return", render_type(clang_getResultType(type)));
    switch (clang_getFunctionTypeCallingConv(type)) {
    case CXCallingConv_C:
      json_object_set_new(js, "calling_convention", json_string("cdecl"));
      break;
    case CXCallingConv_X86StdCall:
      json_object_set_new(js, "calling_convention", json_string("stdcall"));
      break;
    case CXCallingConv_X86FastCall:
      json_object_set_new(js, "calling_convention", json_string("fastcall"));
      break;
    case CXCallingConv_X86ThisCall:
      json_object_set_new(js, "calling_convention", json_string("thiscall"));
      break;
    case CXCallingConv_X86Pascal:
      json_object_set_new(js, "calling_convention", json_string("pascal"));
      break;
    case CXCallingConv_AAPCS:
      json_object_set_new(js, "calling_convention", json_string("aapcs"));
      break;
    case CXCallingConv_AAPCS_VFP:
      json_object_set_new(js, "calling_convention", json_string("aapcs-vfp"));
      break;
    default:
      break;
    }
    json_t* arguments = json_array();
    for (i=0; i<clang_getNumArgTypes(type); i++) {
      json_array_append_new(arguments, render_type(clang_getArgType(type, i)));
    }
    json_object_set_new(js, "arguments", arguments);
    return js;

  default:
    if (clang_getCursorKind(decl) != CXCursor_NoDeclFound) {
      json_object_set_new(js, "kind", json_string("ref"));
      json_object_set_new(js, "id", render_string(clang_getCursorUSR(decl)));
    } else {
      int found = 0, i;
      for (i=0; i<sizeof(PRIMITIVE_TYPES)/sizeof(PRIMITIVE_TYPES[0]); i++) {
        if (PRIMITIVE_TYPES[i].kind == kind) {
          json_object_set_new(js, "kind", json_string("primitive"));
          json_object_set_new(js, "primitive", json_string(PRIMITIVE_TYPES[i].name));
          found = 1;
          break;
        }
      }
      if (!found) {
        json_object_set_new(js, "kind", json_string("unknown"));
        json_object_set_new(js, "clang_kind", render_string(clang_getTypeKindSpelling(kind)));
      }
    }
    return js;
  }
}


enum CXChildVisitResult visit_enum(CXCursor cursor, CXCursor parent, CXClientData data) {
  json_t* structure = (json_t*)data;
  json_t* js = NULL;
  switch (clang_getCursorKind(cursor)) {
  case CXCursor_EnumConstantDecl:
    js = json_object();
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    json_object_set_new(js, "value", json_integer(clang_getEnumConstantDeclValue(cursor)));
    json_object_set_new(js, "type", render_type(clang_getCursorType(cursor)));
    json_array_append_new(json_object_get(structure, "values"), js);
    break;
  default:
    break;
  }
  return CXChildVisit_Continue;
}

enum CXChildVisitResult visit_structure(CXCursor cursor, CXCursor parent, CXClientData data) {
  json_t* structure = (json_t*)data;
  json_t* js = NULL;
  switch (clang_getCursorKind(cursor)) {
  case CXCursor_FieldDecl:
    js = json_object();
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    json_object_set_new(js, "type", render_type(clang_getCursorType(cursor)));
    json_array_append_new(json_object_get(structure, "fields"), js);
    break;
  case CXCursor_IntegerLiteral:
    break;
  default:
    break;
  }
  return CXChildVisit_Continue;
}


enum CXChildVisitResult visit_object(CXCursor cursor, CXCursor parent, CXClientData data) {
  json_t* object = (json_t*)data;
  json_t* arguments = json_object_get(object, "argument_names");
  switch (clang_getCursorKind(cursor)) {
  case CXCursor_ParmDecl:
    if (arguments) {
      json_array_append_new(arguments, render_string_or_null(clang_getCursorSpelling(cursor)));
    }
    break;
  case CXCursor_IntegerLiteral:
  case CXCursor_FloatingLiteral:
  case CXCursor_StringLiteral:
    //putstring(clang_getCursorSpelling(cursor));
    //putstring(clang_getCursorDisplayName(cursor));
    //printf("%d\n", clang_getEnumConstantDeclValue(cursor));
    break;
  default:
    break;
  }
  return CXChildVisit_Continue;
}


void dump_tokens(CXCursor cursor) {

}



enum CXChildVisitResult visit_program(CXCursor cursor, CXCursor parent, CXClientData data){
  json_t* program = (json_t*)data;
  json_t* js = NULL;
  enum CXChildVisitResult ret = CXChildVisit_Continue;
  switch (clang_getCursorKind(cursor)) {
  case CXCursor_FunctionDecl:
  case CXCursor_VarDecl:
    js = json_object();
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    json_object_set_new(js, "display_name", render_string(clang_getCursorDisplayName(cursor)));
    json_object_set_new(js, "type", render_type(clang_getCursorType(cursor)));
    if (clang_getCursorKind(cursor) == CXCursor_FunctionDecl) {
      json_object_set_new(js, "argument_names", json_array());
      json_object_set_new(js, "kind", json_string("function"));
    } else {
      json_object_set_new(js, "kind", json_string("variable"));
    }
    switch (clang_getCursorLinkage(cursor)) {
    case CXLinkage_Internal:
      json_object_set_new(js, "linkage", json_string("static"));
      break;
    case CXLinkage_UniqueExternal:
      json_object_set_new(js, "linkage", json_string("anonymous"));
      break;
    case CXLinkage_External:
      break;
    default:
      json_object_set_new(js, "linkage", json_string("unknown"));
    }
    clang_visitChildren(cursor, visit_object, (CXClientData)js);
    
    break;
    
  case CXCursor_StructDecl:
  case CXCursor_UnionDecl:
    js = json_object();
    json_object_set_new(js, "kind", json_string(clang_getCursorKind(cursor) == CXCursor_UnionDecl ? 
                                                "union" : "struct"));
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    json_object_set_new(js, "fields", json_array());
    clang_visitChildren(cursor, visit_structure, (CXClientData)js);
    ret = CXChildVisit_Recurse;
    break;

  case CXCursor_EnumDecl:
    js = json_object();
    json_object_set_new(js, "kind", json_string("enum"));
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    json_object_set_new(js, "values", json_array());
    clang_visitChildren(cursor, visit_enum, (CXClientData)js);
    break;

  case CXCursor_TypedefDecl:
    js = json_object();
    json_object_set_new(js, "kind", json_string("typedef"));
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    json_object_set_new(js, "type", render_type(clang_getTypedefDeclUnderlyingType(cursor)));
    ret = CXChildVisit_Recurse;
    break;


  case CXCursor_FieldDecl:
    break;

  case CXCursor_MacroDefinition:
    js = json_object();
    json_object_set_new(js, "kind", json_string("macro"));
    {
      CXToken* tokens;
      unsigned ntokens, i;
      CXString name = clang_getCursorSpelling(cursor);
      char value_buf[1024] = "";
      json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
      clang_tokenize(TU, clang_getCursorExtent(cursor), &tokens, &ntokens);
      for (i=0; i<ntokens; i++) {
        CXString str = clang_getTokenSpelling(TU, tokens[i]);
        CXTokenKind tkind = clang_getTokenKind(tokens[i]);
        if (i == 0 && !strcmp(clang_getCString(name), clang_getCString(str))) {
          // macro name
        } else if (i == ntokens - 1 && 
                   tkind == CXToken_Punctuation && !strcmp("#", clang_getCString(str))) {
          // weird clang terminator thingy
        } else if (tkind == CXToken_Comment) {
          // comment
        } else {
          if (strlen(value_buf) > 0) {
            strncat(value_buf, " ", sizeof(value_buf) - strlen(value_buf) - 1);
          }
          strncat(value_buf, clang_getCString(str), sizeof(value_buf) - strlen(value_buf) - 1);
        }
        clang_disposeString(str);
      }
      clang_disposeTokens(TU, tokens, ntokens);
      if (strlen(value_buf) > 0) {
        long int intval;
        double dblval;
        char* endptr_int;
        char* endptr_dbl;
        intval = strtol(value_buf, &endptr_int, 0);
        dblval = strtod(value_buf, &endptr_dbl);
        if (endptr_int[0] == 0 ||
            (endptr_int[1] == 0 && strchr("UuLl", endptr_int[0]) != NULL)) {
          json_object_set_new(js, "value", json_integer(intval));
        } else if (endptr_dbl[0] == 0 || 
                   (endptr_dbl[1] == 0 && strchr("fF", endptr_dbl[0]) != NULL)) {
          json_object_set_new(js, "value", json_real(dblval));
        } else {
          json_object_set_new(js, "value", json_string(value_buf));
        }
      }
    }
    break;

    /*
  case CXCursor_PreprocessingDirective:
  case CXCursor_MacroExpansion:
    js = json_object();
    json_object_set_new(js, "kind", json_string("macro"));
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    putstring(clang_getCursorSpelling(cursor));
    putstring(clang_getCursorDisplayName(cursor));
    printf("%d\n", clang_getEnumConstantDeclValue(cursor));
    
    ret = CXChildVisit_Recurse;
    */

  default:
    js = json_object();
    json_object_set_new(js, "name", render_string(clang_getCursorSpelling(cursor)));
    json_object_set_new(js, "type", render_type(clang_getCursorType(cursor)));
    json_object_set_new(js, "kind", json_string("wtf"));
    json_object_set_new(js, "wtf", render_string(clang_getCursorKindSpelling(clang_getCursorKind(cursor))));
  }

  if (js) {
    json_t* str;
    if (!json_object_get(program, "")) json_object_set_new(program, "", json_array());
    str = render_string(clang_getCursorUSR(cursor));
    if (strlen(json_string_value(str)) == 0) {
      json_decref(str);
      json_array_append_new(json_object_get(program, ""), js);
    } else {
      json_object_set_with_key_new(program, render_string(clang_getCursorUSR(cursor)), js);
    }
  }

  return ret;
}


int main(int argc, const char* const argv[]){
  if (argc < 2) {
    printf("Usage: %s header.h\n", argv[0]);
    printf("Standard clang arguments (-I, -D, etc.) may be used\n");
    exit(1);
  }
  unsigned i;

  char filename[] = "ffigen.tmp.XXXXXX";
  int fd = mkstemp(filename);
  FILE* file = fdopen(fd, "w");
  fprintf(file, "#define _SIZE_T\n");
  fprintf(file, "#define _PTRDIFF_T\n");
  fprintf(file, "typedef __SIZE_TYPE__ size_t;\n");
  fprintf(file, "typedef __PTRDIFF_TYPE__ ptrdiff_t;\n");
  fprintf(file, "#include <%s>\n", argv[1]);
  fclose(file);

  int clang_argc = argc + 1;
  const char** clang_argv = malloc(sizeof(char*) * clang_argc);
  clang_argv[0] = "-x";
  clang_argv[1] = "c";
  clang_argv[2] = filename;
  for (i=3; i < clang_argc; i++) {
    clang_argv[i] = argv[i-1];
  }

  CXIndex Index = clang_createIndex(0, 0);
  TU = clang_parseTranslationUnit(Index, 0, clang_argv, clang_argc,
                                  0, 0, CXTranslationUnit_DetailedPreprocessingRecord);
  json_t* json = json_object();
  clang_visitChildren(clang_getTranslationUnitCursor(TU), visit_program, (CXClientData)json);


  json_dumpf(json, stdout, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
  printf("\n");
  json_decref(json);


  for (i=0; i<clang_getNumDiagnostics(TU); i++) {
    putstring(clang_formatDiagnostic(clang_getDiagnostic(TU, i), clang_defaultDiagnosticDisplayOptions()));
  }


  clang_disposeTranslationUnit(TU);
  clang_disposeIndex(Index);
  free(clang_argv);
  unlink(filename);

  return 0;
}
