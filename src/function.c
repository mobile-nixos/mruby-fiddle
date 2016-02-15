#include "fiddle.h"

#ifdef PRIsVALUE
# define RB_OBJ_CLASSNAME(obj) rb_obj_class(obj)
# define RB_OBJ_STRING(obj) (obj)
#else
# define PRIsVALUE "s"
# define RB_OBJ_CLASSNAME(obj) rb_obj_classname(obj)
# define RB_OBJ_STRING(obj) StringValueCStr(obj)
#endif

mrb_value function;

#define MAX_ARGS (SIZE_MAX / (sizeof(void *) + sizeof(fiddle_generic)) - 1)

#define Check_Max_Args(name, len) \
    if ((size_t)(len) < MAX_ARGS) { \
	/* OK */ \
    } \
    else { \
	rb_raise(rb_eTypeError, \
		 name" is so large that it can cause integer overflow (%d)", \
		 (len)); \
    }

static void
mrb_function_free(mrb_state *mrb, void *p)
{
    ffi_cif *ptr = p;
    if (ptr->arg_types) mrb_free(ptr->arg_types);
    mrb_free(ptr);
}

static const struct mrb_data_type function_data_type = {
    "fiddle/function",
    mrb_function_free,
};

static mrb_value
mrb_function_allocate(mrb_state *mrb, mrb_value klass)
{
    ffi_cif * cif;
    struct RData *data;

    Data_Make_Struct(mrb, klass, ffi_cif, &function_data_type, cif, data);

    return mrb_obj_value(data);
}

static mrb_value
mrb_function_initialize(mrb_state *mrb, mrb_value self)
{
    ffi_cif * cif;
    ffi_type **arg_types;
    ffi_status result;
    mrb_value ptr, args, ret_type, abi, name;
    mrb_int i, args_len;

    mrb_get_args(mrb, "oA|oiS", &ptr, &args, &ret_type, &abi, &name);
    if (mrb_nil_p(ret_type)) ret_type = mrb_fixnum_value(TYPE_VOID);
    if (mrb_nil_p(abi)) abi = mrb_fixnum_value(FFI_DEFAULT_ABI);
    if (!mrb_nil_p(name)) mrb_iv_set(mrb, self, "@name", name);

    mrb_iv_set(mrb, self, "@ptr", ptr);
    mrb_iv_set(mrb, self, "@args", args);
    mrb_iv_set(mrb, self, "@return_type", ret_type);
    mrb_iv_set(mrb, self, "@abi", abi);

    Data_Get_Struct(mrb, self, &function_data_type, cif);

    args_len = mrb_ary_len(mrb, args);

    arg_types = mrb_calloc(mrb, args_len + 1, sizeof(ffi_type *));

    for (i = 0; i < args_len; i++) {
    	mrb_int type = mrb_fixnum(mrb_ary_entry(args, i));
    	arg_types[i] = INT2FFI_TYPE(type);
    }
    arg_types[args_len] = NULL;

    result = ffi_prep_cif (
	    cif,
	    mrb_fixnum(abi),
	    args_len,
	    INT2FFI_TYPE(fixnum(ret_type)),
	    arg_types);

    if (result)
	   mrb_raisef(mrb, E_RUNTIME_ERROR, "error creating CIF %d", result);

    return self;
}

static mrb_value
mrb_function_call(mrb_state *mrb, mrb_value self)
{
    ffi_cif * cif;
    fiddle_generic retval;
    fiddle_generic *generic_args;
    void **values;
    mrb_value cfunc, types, pointer;
    int i;
    mrb_value alloc_buffer = 0;
    struct *RClass fiddle;

    cfunc    = mrb_iv_get(mrb, self, "@ptr");
    types    = mrb_iv_get(mrb, self, "@args");
    pointer = mrb_const_get(mrb, fiddle, rb_intern("Pointer"));

    Check_Max_Args("number of arguments", argc);
    if(argc != RARRAY_LENINT(types)) {
	rb_raise(rb_eArgError, "wrong number of arguments (%d for %d)",
		argc, RARRAY_LENINT(types));
    }

    TypedData_Get_Struct(self, ffi_cif, &function_data_type, cif);

    if (rb_safe_level() >= 1) {
	for (i = 0; i < argc; i++) {
	    mrb_value src = argv[i];
	    if (OBJ_TAINTED(src)) {
		rb_raise(rb_eSecurityError, "tainted parameter not allowed");
	    }
	}
    }

    generic_args = ALLOCV(alloc_buffer,
	(size_t)(argc + 1) * sizeof(void *) + (size_t)argc * sizeof(fiddle_generic));
    values = (void **)((char *)generic_args + (size_t)argc * sizeof(fiddle_generic));

    for (i = 0; i < argc; i++) {
	mrb_value type = RARRAY_PTR(types)[i];
	mrb_value src = argv[i];

	if(NUM2INT(type) == TYPE_VOIDP) {
	    if(NIL_P(src)) {
		src = INT2FIX(0);
	    } else if(cPointer != CLASS_OF(src)) {
		src = rb_funcall(cPointer, rb_intern("[]"), 1, src);
	    }
	    src = rb_Integer(src);
	}

	VALUE2GENERIC(NUM2INT(type), src, &generic_args[i]);
	values[i] = (void *)&generic_args[i];
    }
    values[argc] = NULL;

    ffi_call(cif, NUM2PTR(rb_Integer(cfunc)), &retval, values);

    rb_funcall(mFiddle, rb_intern("last_error="), 1, mrb_fixnum_value(errno));
#if defined(_WIN32)
    rb_funcall(mFiddle, rb_intern("win32_last_error="), 1, mrb_fixnum_value(errno));
#endif

    ALLOCV_END(alloc_buffer);

    return GENERIC2VALUE(rb_iv_get(self, "@return_type"), retval);
}

void
mrb_fiddle_function_init(mrb_state *mrb, struct RClass *fiddle)
{
    struct RClass *function;
    /*
     * Document-class: Fiddle::Function
     *
     * == Description
     *
     * A representation of a C function
     *
     * == Examples
     *
     * === 'strcpy'
     *
     *   @libc = Fiddle.dlopen "/lib/libc.so.6"
     *	    #=> #<Fiddle::Handle:0x00000001d7a8d8>
     *   f = Fiddle::Function.new(
     *     @libc['strcpy'],
     *     [Fiddle::TYPE_VOIDP, Fiddle::TYPE_VOIDP],
     *     Fiddle::TYPE_VOIDP)
     *	    #=> #<Fiddle::Function:0x00000001d8ee00>
     *   buff = "000"
     *	    #=> "000"
     *   str = f.call(buff, "123")
     *	    #=> #<Fiddle::Pointer:0x00000001d0c380 ptr=0x000000018a21b8 size=0 free=0x00000000000000>
     *   str.to_s
     *   => "123"
     *
     * === ABI check
     *
     *   @libc = Fiddle.dlopen "/lib/libc.so.6"
     *	    #=> #<Fiddle::Handle:0x00000001d7a8d8>
     *   f = Fiddle::Function.new(@libc['strcpy'], [TYPE_VOIDP, TYPE_VOIDP], TYPE_VOIDP)
     *	    #=> #<Fiddle::Function:0x00000001d8ee00>
     *   f.abi == Fiddle::Function::DEFAULT
     *	    #=> true
     */
    function = mrb_define_class_under(mrb, fiddle, "Function", mrb->object_class);

    /*
     * Document-const: DEFAULT
     *
     * Default ABI
     *
     */
    mrb_define_const(mrb, function, "DEFAULT", mrb_fixnum_value(FFI_DEFAULT_ABI));

#ifdef HAVE_CONST_FFI_STDCALL
    /*
     * Document-const: STDCALL
     *
     * FFI implementation of WIN32 stdcall convention
     *
     */
    mrb_define_const(mrb, function, "STDCALL", mrb_fixnum_value(FFI_STDCALL));
#endif

    rb_define_alloc_func(function, allocate);

    /*
     * Document-method: call
     *
     * Calls the constructed Function, with +args+
     *
     * For an example see Fiddle::Function
     *
     */
    mrb_define_method(mrb, function, "call", function_call, -1);

    /*
     * Document-method: new
     * call-seq: new(ptr, args, ret_type, abi = DEFAULT)
     *
     * Constructs a Function object.
     * * +ptr+ is a referenced function, of a Fiddle::Handle
     * * +args+ is an Array of arguments, passed to the +ptr+ function
     * * +ret_type+ is the return type of the function
     * * +abi+ is the ABI of the function
     *
     */
    mrb_define_method(mrb, function, "initialize", mrb_function_initialize, MRB_ARGS_ARG(2, 3));
}
/* vim: set noet sws=4 sw=4: */
