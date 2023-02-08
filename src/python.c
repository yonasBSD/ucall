/**
 * @file python.c
 * @author Ashot Vardanian
 * @date 2023-01-30
 * @copyright Copyright (c) 2023
 *
 * @brief Pure CPython bindings for UJRPC.
 *
 * @see Reading Materials
 * https://pythoncapi.readthedocs.io/type_object.html
 * https://numpy.org/doc/stable/reference/c-api/types-and-structures.html
 * https://pythonextensionpatterns.readthedocs.io/en/latest/refcount.html
 * https://docs.python.org/3/extending/newtypes_tutorial.html#adding-data-and-methods-to-the-basic-example
 */
#define PY_SSIZE_T_CLEAN
#include "helpers/py_parse.hpp"
#include <Python.h>
#include <time.h>

#include "ujrpc/ujrpc.h"

const size_t max_response_length = 1024;

typedef struct {
    PyObject_HEAD;
    ujrpc_config_t config;
    ujrpc_server_t server;
    size_t count_added;
    size_t thread_cnt;
} py_server_t;

typedef enum {
    POSITIONAL_ONLY,       //
    POSITIONAL_OR_KEYWORD, //
    VAR_POSITIONAL,        //
    KEYWORD_ONLY,          //
    VAR_KEYWORD            //
} py_param_kind_t;

typedef struct {
    PyObject* name;       // UTF8 String
    PyObject* value;      // Any or NULL
    PyTypeObject* type;   // Type or NULL
    py_param_kind_t kind; // Kind
} py_param_t;

typedef struct {
    py_param_t* u_params;
    size_t params_cnt;
    PyObject* callable;
} py_wrapper_t;

static py_wrapper_t wrap;

static int deduce_parameters(PyObject* callable) {
    // TODO Add safety checks
    PyObject* func_code = PyObject_GetAttrString(callable, "__code__");

    PyObject* arg_names = PyObject_GetAttrString(func_code, "co_varnames"); // Tuple

    long co_flags = PyLong_AsLong(PyObject_GetAttrString(func_code, "co_flags"));
    long pos_count = PyLong_AsLong(PyObject_GetAttrString(func_code, "co_argcount"));
    long posonly_count = PyLong_AsLong(PyObject_GetAttrString(func_code, "co_posonlyargcount"));
    long keyword_only_count = PyLong_AsLong(PyObject_GetAttrString(func_code, "co_kwonlyargcount"));
    long pos_default_count = 0;

    PyObject* annotations = PyFunction_GetAnnotations(callable); // Dict

    PyObject* defaults = PyObject_GetAttrString(callable, "__defaults__");     // Tuple
    PyObject* kwdefaults = PyObject_GetAttrString(callable, "__kwdefaults__"); // Dict

    if (PyTuple_CheckExact(defaults))
        pos_default_count = PyTuple_Size(defaults);

    if (PyMethod_Check(callable)) {
        // When this is a class method...
        // ToDo What?
    }

    long non_default_count = pos_count - pos_default_count;
    long posonly_left = posonly_count;

    // if (posonly_count != pos_count) {
    //     PyErr_SetString(PyExc_TypeError, "Strictly positional or Keyword arguments are allowed.");
    //     return -1;
    // }

    long total_params = pos_count + (co_flags & CO_VARARGS) + keyword_only_count + (co_flags & CO_VARKEYWORDS);
    py_param_t* parameters = (py_param_t*)malloc(total_params * sizeof(py_param_t));

    size_t param_i = 0;

    // Non-keyword-only parameters w/o defaults.
    for (Py_ssize_t i = 0; i < non_default_count; ++i) {
        py_param_t param;
        param.name = PyTuple_GetItem(arg_names, i);
        param.value = NULL;
        param.type = (PyTypeObject*)PyDict_GetItem(annotations, param.name);
        param.kind = posonly_left-- > 0 ? POSITIONAL_ONLY : POSITIONAL_OR_KEYWORD;
        parameters[param_i++] = param;
    }

    //... w / defaults.
    for (Py_ssize_t i = non_default_count; i < pos_count; ++i) {
        py_param_t param;
        param.name = PyTuple_GetItem(arg_names, i);
        param.value = PyTuple_GetItem(defaults, i - non_default_count);
        param.type = (PyTypeObject*)PyDict_GetItem(annotations, param.name);
        param.kind = posonly_left-- > 0 ? POSITIONAL_ONLY : POSITIONAL_OR_KEYWORD;
        parameters[param_i++] = param;
    }

    if (co_flags & CO_VARARGS) {
        py_param_t param;
        param.name = PyTuple_GetItem(arg_names, pos_count + keyword_only_count);
        param.value = NULL;
        param.type = (PyTypeObject*)PyDict_GetItem(annotations, param.name);
        param.kind = VAR_POSITIONAL;
        parameters[param_i++] = param;
    }

    // Keyword - only parameters.
    for (Py_ssize_t i = pos_count; i < pos_count + keyword_only_count; ++i) {
        py_param_t param;
        param.name = PyTuple_GetItem(arg_names, i);
        param.value = PyDict_GetItem(kwdefaults, param.name);
        param.type = (PyTypeObject*)PyDict_GetItem(annotations, param.name);
        param.kind = KEYWORD_ONLY;
        parameters[param_i++] = param;
    }

    if (co_flags & CO_VARKEYWORDS) {
        py_param_t param;
        param.name = PyTuple_GetItem(arg_names, pos_count + keyword_only_count + (co_flags && CO_VARARGS));
        param.value = NULL;
        param.type = (PyTypeObject*)PyDict_GetItem(annotations, param.name);
        param.kind = VAR_KEYWORD;
        parameters[param_i++] = param;
    }

    free(wrap.u_params);
    wrap.u_params = parameters;
    wrap.params_cnt = total_params;
    return 0;
}

static void* wrapper(ujrpc_call_t* call) { //
    // TODO Add error checking for `ujrpc_param_named...`
    PyObject* args = PyTuple_New(wrap.params_cnt);

    for (size_t i = 0; i < wrap.params_cnt; ++i) {
        PyObject* type = wrap.u_params[i].type;
        py_param_kind_t kind = wrap.u_params[i].kind;
        bool may_have_name = (kind & POSITIONAL_OR_KEYWORD) | (kind & KEYWORD_ONLY) | (kind & VAR_KEYWORD);
        bool may_have_pos = (kind & POSITIONAL_OR_KEYWORD) | (kind & POSITIONAL_ONLY) | (kind & VAR_POSITIONAL);
        bool got_named = false;
        Py_ssize_t name_len = -1;
        ujrpc_str_t name = "";
        if (may_have_name)
            name =
                PyUnicode_AsUTF8AndSize(wrap.u_params[i].name, &name_len); // TODO do this once in `deduce parameters`;

        if (PyType_IsSubtype(type, &PyBool_Type)) {
            bool res;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_bool(call, name, name_len, &res);
            if (may_have_pos && !got_named)
                ujrpc_param_positional_bool(call, i, &res);
            PyTuple_SetItem(args, i, res ? Py_True : Py_False);
        } else if (PyType_IsSubtype(type, &PyLong_Type)) {
            Py_ssize_t res;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_i64(call, name, name_len, &res);
            if (may_have_pos && !got_named)
                ujrpc_param_positional_i64(call, i, &res);
            PyTuple_SetItem(args, i, PyLong_FromSsize_t(res));
        } else if (PyType_IsSubtype(type, &PyFloat_Type)) {
            double res;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_f64(call, name, name_len, &res);
            if (may_have_pos && !got_named)
                ujrpc_param_positional_f64(call, i, &res);
            PyTuple_SetItem(args, i, PyFloat_FromDouble(res));
        } else if (PyType_IsSubtype(type, &PyBytes_Type)) {
            // Pass
        } else if (PyType_IsSubtype(type, &PyUnicode_Type)) {
            ujrpc_str_t res;
            size_t len;
            if (may_have_name && name_len > 0)
                got_named = ujrpc_param_named_str(call, name, name_len, &res, &len);
            if (may_have_pos && !got_named)
                ujrpc_param_positional_str(call, i, res, &len);
            PyTuple_SetItem(args, i, PyUnicode_FromStringAndSize(res, len));
        }
    }

    PyObject* response = PyObject_CallObject(wrap.callable, args);
    char* parsed_response = (char*)malloc(max_response_length * sizeof(char));
    size_t len = 0;
    int res = to_string(response, parsed_response, &len);
    ujrpc_call_reply_content(call, parsed_response, len);
}

static PyObject* server_add_procedure(py_server_t* self, PyObject* args) {
    // Take a function object, introspect its arguments,
    // register them inside of a higher-level function,
    // which on every call requests them via `ujrpc_param_named_...`
    // from the `ujrpc_call_t` context, then wraps them into native
    // Python objects and passes to the original function.
    // The result of that function call must then be returned via
    // the `ujrpc_call_send_content` call.
    PyObject* procedure;
    if (!PyArg_ParseTuple(args, "O", &procedure) || !PyCallable_Check(procedure)) {
        PyErr_SetString(PyExc_TypeError, "Need a callable object!");
        return NULL;
    }
    if (deduce_parameters(procedure) != 0)
        return NULL;
    wrap.callable = procedure;
    ujrpc_add_procedure(self->server, PyUnicode_AsUTF8(PyObject_GetAttrString(procedure, "__name__")), wrapper);
    return Py_None;
}

static PyObject* server_run(py_server_t* self, PyObject* args) {
    Py_ssize_t max_cycles;
    double max_seconds;
    if (!PyArg_ParseTuple(args, "nd", &max_cycles, &max_seconds)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a cycle count and timeout.");
        return NULL;
    }

    time_t start, end;
    time(&start);
    while (max_cycles > 0 && max_seconds > 0) {
        ujrpc_take_call(self->server, self->thread_cnt);
        --max_cycles;
        time(&end);
        max_seconds -= difftime(end, start);
        start = end;
    }
    return Py_None;
}

static Py_ssize_t server_callbacks_count(py_server_t* self, PyObject* _) { return self->count_added; }
static PyObject* server_port(py_server_t* self, PyObject* _) { return PyLong_FromLong(self->config.port); }
static PyObject* server_queue_depth(py_server_t* self, PyObject* _) { return 0; }
static PyObject* server_max_lifetime(py_server_t* self, PyObject* _) { return 0; }

static PyMethodDef server_methods[] = {
    {"add_procedure", (PyCFunction)&server_add_procedure, METH_VARARGS, PyDoc_STR("Append a procedure callback")},
    {"run", (PyCFunction)&server_run, METH_VARARGS,
     PyDoc_STR("Runs the server for N calls or T seconds, before returning")},
    {NULL},
};

static PyGetSetDef server_computed_properties[] = {
    {"port", (getter)&server_port, NULL, PyDoc_STR("On which port the server listens")},
    {"queue_depth", (getter)&server_queue_depth, NULL, PyDoc_STR("Max number of concurrent users")},
    {"max_lifetime", (getter)&server_max_lifetime, NULL, PyDoc_STR("Max lifetime of connections in microseconds")},
    {NULL},
};

static PyMappingMethods server_mapping_methods = {
    .mp_length = (lenfunc)server_callbacks_count,
    .mp_subscript = NULL,
    .mp_ass_subscript = NULL,
};

static void server_dealloc(py_server_t* self) {
    ujrpc_free(self->server);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* server_new(PyTypeObject* type, PyObject* args, PyObject* keywords) {
    py_server_t* self = (py_server_t*)type->tp_alloc(type, 0);
    return (PyObject*)self;
}

static int server_init(py_server_t* self, PyObject* args, PyObject* keywords) {
    static char const* keywords_list[] = {"port", "queue_depth", "max_callbacks", "batch_capacity"};
    Py_ssize_t port = 0, queue_depth = 0, max_callbacks = UINT64_MAX, batch_capacity = UINT64_MAX;
    char const* dtype = NULL;
    char const* metric = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, keywords, "nn|nss", (char**)keywords_list, //
                                     &port, &queue_depth, &max_callbacks, &batch_capacity, &dtype, &metric))
        return -1;

    self->config.port = port;
    self->config.queue_depth = queue_depth;
    self->config.max_callbacks = max_callbacks;
    self->thread_cnt = 1;

    // Initialize the server
    ujrpc_init(&self->config, &self->server);
}

// Order: https://docs.python.org/3/c-api/typeobj.html#quick-reference
static PyTypeObject ujrpc_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "ujrpc.Server",
    .tp_basicsize = sizeof(py_server_t),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)server_dealloc,
    // TODO:
    // .tp_vectorcall_offset = 0,
    // .tp_repr = NULL,
    .tp_as_mapping = &server_mapping_methods,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc = PyDoc_STR("Server class for Remote Procedure Calls implemented in Python"),
    .tp_methods = server_methods,
    .tp_getset = server_computed_properties,
    .tp_init = (initproc)server_init,
    .tp_new = server_new,
};

static PyModuleDef server_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ujrpc",
    .m_doc = "Uninterrupted JSON Remote Procedure Calls library.",
    .m_size = -1,
};

PyMODINIT_FUNC PyInit_ujrpc(void) {
    if (PyType_Ready(&ujrpc_type) < 0)
        return NULL;

    PyObject* m = PyModule_Create(&server_module);
    if (!m)
        return NULL;

    Py_INCREF(&ujrpc_type);
    if (PyModule_AddObject(m, "Server", (PyObject*)&ujrpc_type) < 0) {
        Py_DECREF(&ujrpc_type);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

int main(int argc, char* argv[]) {
    wchar_t* program = Py_DecodeLocale(argv[0], NULL);
    if (!program) {
        fprintf(stderr, "Fatal error: cannot decode argv[0]\n");
        exit(1);
    }

    /* Add a built-in module, before Py_Initialize */
    if (PyImport_AppendInittab("ujrpc", PyInit_ujrpc) == -1) {
        fprintf(stderr, "Error: could not extend in-built modules table\n");
        exit(1);
    }

    /* Pass argv[0] to the Python interpreter */
    Py_SetProgramName(program);

    /* Initialize the Python interpreter.  Required.
       If this step fails, it will be a fatal error. */
    Py_Initialize();

    /* Optionally import the module; alternatively,
       import can be deferred until the embedded script
       imports it. */
    PyObject* pmodule = PyImport_ImportModule("ujrpc");
    if (!pmodule) {
        PyErr_Print();
        fprintf(stderr, "Error: could not import module 'ujrpc'\n");
    }
    PyMem_RawFree(program);
    return 0;
}