/*
 * sand_hacks.c
 *
 * Let's keep all the hacked versions of rb_* equivalents in here.
 *
 * $Author$
 * $Date$
 *
 * Copyright (C) 2006 why the lucky stiff
 */
#include "sand_table.h"
 
VALUE
sandbox_module_new(kit)
  sandkit *kit;
{
  NEWOBJ(mdl, struct RClass);
  OBJSETUP(mdl, kit->cModule, T_MODULE);
  
  mdl->super = 0;
  mdl->iv_tbl = 0;
  mdl->m_tbl = 0;
  mdl->m_tbl = st_init_numtable();
  
  return (VALUE)mdl;
}

VALUE
sandbox_dummy()
{
    return Qnil;
}

VALUE
sandbox_define_module_id(kit, id)
  sandkit *kit;
  ID id;
{   
  VALUE mdl;

  mdl = sandbox_module_new(kit);
  rb_name_class(mdl, id);

  return mdl;
}

VALUE
sandbox_boot(kit, super)
  sandkit *kit;
  VALUE super;
{
  NEWOBJ(klass, struct RClass);
  OBJSETUP(klass, kit->cClass, T_CLASS);
      
  klass->super = super;
  klass->iv_tbl = 0;
  klass->m_tbl = 0;       /* safe GC */
  klass->m_tbl = st_init_numtable();
  
  OBJ_INFECT(klass, super);
  return (VALUE)klass;
}

VALUE
sandbox_metaclass(kit, obj, super)
  sandkit *kit;
  VALUE obj, super;
{
  VALUE klass = sandbox_boot(kit, super);
  FL_SET(klass, FL_SINGLETON);
  RBASIC(obj)->klass = klass;
  rb_singleton_class_attached(klass, obj);
  if (BUILTIN_TYPE(obj) == T_CLASS && FL_TEST(obj, FL_SINGLETON)) {
    RBASIC(klass)->klass = klass;
    RCLASS(klass)->super = RBASIC(rb_class_real(RCLASS(obj)->super))->klass;
  }
  else {
    VALUE metasuper = RBASIC(rb_class_real(super))->klass;

    /* metaclass of a superclass may be NULL at boot time */
    if (metasuper) {
        RBASIC(klass)->klass = metasuper;
    }
  }

  return klass;
}

VALUE
sandbox_singleton_class(kit, obj)
  sandkit *kit;
  VALUE obj;
{
  VALUE klass;

  if (FIXNUM_P(obj) || SYMBOL_P(obj)) {
    rb_raise(rb_eTypeError, "can't define singleton");
  }
  if (rb_special_const_p(obj)) {
    rb_raise(rb_eTypeError, "no special constants in the sandbox");
  }

  DEFER_INTS;
  if (FL_TEST(RBASIC(obj)->klass, FL_SINGLETON) &&
  rb_iv_get(RBASIC(obj)->klass, "__attached__") == obj) {
    klass = RBASIC(obj)->klass;
  }
  else {
    klass = sandbox_metaclass(kit, obj, RBASIC(obj)->klass);
  }
  if (OBJ_TAINTED(obj)) {
    OBJ_TAINT(klass);
  }
  else {
    FL_UNSET(klass, FL_TAINT);
  }
  if (OBJ_FROZEN(obj)) OBJ_FREEZE(klass);
  ALLOW_INTS;
  
  return klass;
}

VALUE
sandbox_defclass(kit, name, super)
  sandkit *kit;
  const char *name;
  VALUE super;
{   
  VALUE obj = sandbox_boot(kit, super);
  ID id = rb_intern(name);

  rb_name_class(obj, id);
  st_add_direct(kit->tbl, id, obj);
  rb_const_set((kit->cObject ? kit->cObject : obj), id, obj);
  return obj;
}

VALUE
sandbox_defmodule(kit, name)
  sandkit *kit;
  const char *name;
{
  VALUE module;
  ID id;

  id = rb_intern(name);
  if (rb_const_defined(kit->cObject, id)) {
    module = rb_const_get(kit->cObject, id);
    if (TYPE(module) == T_MODULE)
        return module;
    rb_raise(rb_eTypeError, "%s is not a module", rb_obj_classname(module));
  }
  module = sandbox_define_module_id(kit, id);
  st_add_direct(kit->tbl, id, module);
  rb_const_set(kit->cObject, id, module);

  return module;
}

VALUE
sandbox_str(kit, ptr)
  sandkit *kit;
  const char *ptr;
{   
  NEWOBJ(str, struct RString);
  OBJSETUP(str, kit->cString, T_STRING);

  str->len = strlen(ptr);
  str->aux.capa = str->len;
  str->ptr = ALLOC_N(char,str->len+1);
  memcpy(str->ptr, ptr, str->len);
  str->ptr[str->len] = '\0';

  return (VALUE)str;
}


struct trace_var {
  int removed;
  void (*func)();
  VALUE data;
  struct trace_var *next;
};

struct global_variable {
  int   counter;
  void *data;
  VALUE (*getter)();
  void  (*setter)();
  void  (*marker)();
  int block_trace;
  struct trace_var *trace;
};

struct global_entry {
  struct global_variable *var;
  ID id;
};

static VALUE undef_getter();
static void  undef_setter();
static void  undef_marker();

static VALUE val_getter();
static void  val_setter();
static void  val_marker();

static VALUE var_getter();
static void  var_setter();
static void  var_marker();

struct global_entry*
sandbox_global_entry(kit, id)
  sandkit *kit;
  ID id;
{
  struct global_entry *entry;

  if (!st_lookup(kit->globals, id, (st_data_t *)&entry)) {
    struct global_variable *var;
    entry = ALLOC(struct global_entry);
    var = ALLOC(struct global_variable);
    entry->id = id;
    entry->var = var;
    var->counter = 1;
    var->data = 0;
    var->getter = undef_getter;
    var->setter = undef_setter;
    var->marker = undef_marker;

    var->block_trace = 0;
    var->trace = 0;
    st_add_direct(kit->globals, id, (st_data_t)entry);
  }
  return entry;
}

static VALUE
undef_getter(id)
  ID id;
{
  rb_warning("global variable `%s' not initialized", rb_id2name(id));

  return Qnil;
}

static void
undef_setter(val, id, data, var)
  VALUE val;
  ID id;
  void *data;
  struct global_variable *var;
{
  var->getter = val_getter;
  var->setter = val_setter;
  var->marker = val_marker;

  var->data = (void*)val;
}

static void
undef_marker()
{
}

static VALUE
val_getter(id, val)
  ID id;
  VALUE val;
{
  return val;
}

static void
val_setter(val, id, data, var)
  VALUE val;
  ID id;
  void *data;
  struct global_variable *var;
{
  var->data = (void*)val;
}

static void
val_marker(data)
  VALUE data;
{
  if (data) rb_gc_mark_maybe(data);
}

static VALUE
var_getter(id, var)
  ID id;
  VALUE *var;
{
  if (!var) return Qnil;
  return *var;
}

static void
var_setter(val, id, var)
  VALUE val;
  ID id;
  VALUE *var;
{
  *var = val;
}

static void
var_marker(var)
  VALUE *var;
{
  if (var) rb_gc_mark_maybe(*var);
}

static void
readonly_setter(val, id, var)
  VALUE val;
  ID id;
  void *var;
{
  rb_name_error(id, "%s is a read-only variable", rb_id2name(id));
}

static ID
global_id(name)
  const char *name;
{
  ID id;

  if (name[0] == '$') id = rb_intern(name);
  else {
    char *buf = ALLOCA_N(char, strlen(name)+2);
    buf[0] = '$';
    strcpy(buf+1, name);
    id = rb_intern(buf);
  }
  return id;
}

void
sandbox_define_hooked_variable(kit, name, var, getter, setter)
  sandkit *kit;
  const char  *name;
  VALUE *var;
  VALUE (*getter)();
  void  (*setter)();
{
  struct global_variable *gvar;
  ID id = global_id(name);

  gvar = sandbox_global_entry(kit, id)->var;
  gvar->data = (void*)var;
  gvar->getter = getter?getter:var_getter;
  gvar->setter = setter?setter:var_setter;
  gvar->marker = var_marker;
}

void
sandbox_define_variable(kit, name, var)
  sandkit *kit;
  const char  *name;
  VALUE *var;
{
  sandbox_define_hooked_variable(kit, name, var, 0, 0);
}

void
sandbox_define_readonly_variable(kit, name, var)
  sandkit      *kit;
  const char  *name;
  VALUE *var;
{
  sandbox_define_hooked_variable(kit, name, var, 0, readonly_setter);
}

void
sandbox_define_virtual_variable(kit, name, getter, setter)
  sandkit      *kit;
  const char  *name;
  VALUE (*getter)();
  void  (*setter)();
{
  if (!getter) getter = val_getter;
  if (!setter) setter = readonly_setter;
  sandbox_define_hooked_variable(kit, name, 0, getter, setter);
}

static int
sandbox_mark_global(key, entry)
  ID key;
  struct global_entry *entry;
{
  struct trace_var *trace;
  struct global_variable *var = entry->var;

  (*var->marker)(var->data);
  trace = var->trace;
  while (trace) {
    if (trace->data) rb_gc_mark_maybe(trace->data);
    trace = trace->next;
  }
  return ST_CONTINUE;
}

void
sandbox_mark_globals(st_table *tbl)
{   
  if (tbl) {
    st_foreach(tbl, sandbox_mark_global, 0);
  }
}