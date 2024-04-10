#include "Python.h"
#include "pyerrors.h"
#include <stdarg.h>
#include <stdbool.h>

typedef int (*awaitcallback)(PyObject *, PyObject *);
typedef int (*awaitcallback_err)(PyObject *, PyObject *);

typedef struct _PyAwaitableObject PyAwaitableObject;

typedef struct {
    PyObject *coro;
    awaitcallback callback;
    awaitcallback_err err_callback;
    bool done;
} awaitable_callback;

struct _PyAwaitableObject {
    PyObject_HEAD
    awaitable_callback **aw_callbacks;
    Py_ssize_t aw_callback_size;
    PyObject *aw_result;
    PyObject *aw_gen;
    PyObject **aw_values;
    Py_ssize_t aw_values_size;
    void **aw_arb_values;
    Py_ssize_t aw_arb_values_size;
    Py_ssize_t aw_state;
    bool aw_done;
};

typedef struct {
    PyObject_HEAD
    PyObject *gw_result;
    PyAwaitableObject *gw_aw;
    PyObject *gw_current_await;
} GenWrapperObject;

PyDoc_STRVAR(awaitable_doc,
    "Awaitable transport utility for the C API.");

static PyObject *
awaitable_new(PyTypeObject *tp, PyObject *args, PyObject *kwds)
{
    assert(tp != NULL);
    assert(tp->tp_alloc != NULL);

    PyObject *self = tp->tp_alloc(tp, 0);
    if (self == NULL) {
        return NULL;
    }

    PyAwaitableObject *aw = (PyAwaitableObject *) self;
    aw->aw_callbacks = NULL;
    aw->aw_callback_size = 0;
    aw->aw_result = NULL;
    aw->aw_gen = NULL;
    aw->aw_values = NULL;
    aw->aw_values_size = 0;
    aw->aw_state = 0;
    aw->aw_done = false;

    return (PyObject *) aw;
}


static PyObject *
gen_new(PyTypeObject *tp, PyObject *args, PyObject *kwds)
{
    assert(tp != NULL);
    assert(tp->tp_alloc != NULL);

    PyObject *self = tp->tp_alloc(tp, 0);
    if (self == NULL) {
        return NULL;
    }

    GenWrapperObject *g = (GenWrapperObject *) self;
    g->gw_result = NULL;
    g->gw_aw = NULL;
    g->gw_current_await = NULL;

    return (PyObject *) g;
}

static void
gen_dealloc(PyObject *self)
{
    GenWrapperObject *g = (GenWrapperObject *) self;
    Py_XDECREF(g->gw_current_await);
    Py_XDECREF(g->gw_aw);
    Py_XDECREF(g->gw_result);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *
_PyAwaitable_GenWrapper_New(PyAwaitableObject *aw)
{
    assert(aw != NULL);
    GenWrapperObject *g = (GenWrapperObject *) gen_new(
        &_PyAwaitable_GenWrapper_Type,
        NULL,
        NULL
    );

    if (g == NULL) return NULL;
    g->gw_aw = (PyAwaitableObject *) Py_NewRef((PyObject *) aw);
    return (PyObject *) g;
}

static void
_PyAwaitable_GenWrapper_SetResult(PyObject *gen, PyObject *result)
{
    assert(gen != NULL);
    assert(result != NULL);
    Py_INCREF(gen);
    GenWrapperObject *g = (GenWrapperObject *) gen;

    g->gw_result = Py_NewRef(result);
    Py_DECREF(gen);
}

static int
fire_err_callback(PyObject *self, PyObject *await, awaitable_callback *cb)
{
    assert(PyErr_Occurred() != NULL);
    if (!cb->err_callback) {
        cb->done = true;
        Py_DECREF(cb->coro);
        Py_XDECREF(await);
        return -1;
    }

    PyObject *err = PyErr_GetRaisedException();

    if (err == NULL) {
        PyErr_SetString(PyExc_SystemError, "fire_err_callback was called without an exception");
        return -1;
    }

    Py_INCREF(self);
    Py_INCREF(err);
    int e_res = cb->err_callback(self, err);
    cb->done = true;
    Py_DECREF(self);

    if (e_res < 0) {
        if (PyErr_Occurred())
            PyErr_Print();

        if (e_res == -1)
            PyErr_SetRaisedException(err);
        else
            Py_DECREF(err);
        // if the res is -1, the error is restored. otherwise, it is not
        Py_DECREF(cb->coro);
        Py_XDECREF(await);
        return -1;
    };

    Py_DECREF(err);

    return 0;
}

static PyObject *
gen_next(PyObject *self)
{
    GenWrapperObject *g = (GenWrapperObject *) self;
    PyAwaitableObject *aw = g->gw_aw;
    awaitable_callback *cb;

    if (((aw->aw_state + 1) > aw->aw_callback_size) &&
        g->gw_current_await == NULL) {
        PyErr_SetObject(PyExc_StopIteration,
                        g->gw_result ?
                        g->gw_result :
                        Py_None);
        return NULL;
    }
    if (g->gw_current_await == NULL) {
        cb = aw->aw_callbacks[aw->aw_state++];

        if (Py_TYPE(cb->coro)->tp_as_async == NULL ||
            Py_TYPE(cb->coro)->tp_as_async->am_await == NULL) {
            PyErr_Format(PyExc_TypeError, "%R has no __await__", cb->coro);
            return NULL;
        }

        g->gw_current_await = Py_TYPE(cb->coro)->tp_as_async->am_await(
                                                            cb->coro);
        if (g->gw_current_await == NULL) {
            if (fire_err_callback((PyObject *) aw, g->gw_current_await, cb) < 0) {
                return NULL;
            }

            return gen_next(self);
        }
    } else {
        cb = aw->aw_callbacks[aw->aw_state - 1];
    }

    PyObject *result = Py_TYPE(g->gw_current_await
                        )->tp_iternext(g->gw_current_await);

    if (result == NULL) {
        PyObject *occurred = PyErr_GetRaisedException();
        if (!occurred) {
            // coro is done
            g->gw_current_await = NULL;
            return gen_next(self);
        }

        if (!PyErr_GivenExceptionMatches(occurred, PyExc_StopIteration)) {
            if (fire_err_callback((PyObject *) aw, g->gw_current_await, cb) < 0) {
                return NULL;
            }
            Py_DECREF(occurred);
            g->gw_current_await = NULL;
            return gen_next(self);
        }

        if (cb->callback == NULL) {
            // coro is done, but with a result
            // we can disregard the result if theres no callback
            Py_DECREF(occurred);
            g->gw_current_await = NULL;
            return gen_next(self);
        }

        PyObject* value = PyObject_GetAttrString(occurred, "value");
        Py_DECREF(occurred);
        if (value == NULL) {
            return NULL;
        }

        Py_INCREF(aw);
        int result = cb->callback((PyObject *) aw, value);
        Py_DECREF(aw);
        Py_DECREF(value);

        if (result < -1) {
            // -2 or lower denotes that the error should be deferred
            // regardless of whether a handler is present
            return NULL;
        }

        if (result < 0) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_SystemError, "awaitable callback returned -1 without exception set");
                return NULL;
            }
            if (fire_err_callback((PyObject *) aw, g->gw_current_await, cb) < 0) {
                return NULL;
            }
        }

        cb->done = true;
        g->gw_current_await = NULL;
        return gen_next(self);
    }

    return result;
}


static PyObject *
awaitable_next(PyObject *self)
{
    PyAwaitableObject *aw = (PyAwaitableObject *) self;


    if (aw->aw_gen) {
        PyErr_SetString(PyExc_RuntimeError, "cannot reuse C awaitable");
        return NULL;
    }

    PyObject* gen = _PyAwaitable_GenWrapper_New(aw);

    if (gen == NULL) {
        return NULL;
    }

    aw->aw_gen = Py_NewRef(gen);
    return gen;
}

static void
awaitable_dealloc(PyObject *self)
{
    PyAwaitableObject *aw = (PyAwaitableObject *) self;
    if (aw->aw_values) {
        for (int i = 0; i < aw->aw_values_size; i++)
            Py_DECREF(aw->aw_values[i]);
        PyMem_Free(aw->aw_values);
    }

    Py_XDECREF(aw->aw_gen);
    Py_XDECREF(aw->aw_result);

    for (int i = 0; i < aw->aw_callback_size; i++) {
        awaitable_callback *cb = aw->aw_callbacks[i];
        if (!cb->done) Py_DECREF(cb->coro);
        free(cb);
    }

    if (aw->aw_arb_values) PyMem_Free(aw->aw_arb_values);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *
awaitable_repr(PyObject *self) {
    assert(self != NULL);
    PyAwaitableObject *aw = (PyAwaitableObject *) self;
    Py_ssize_t done_size = 0;
    for (int i = 0; i < aw->aw_callback_size; i++) {
        if (aw->aw_callbacks[i]->done) ++done_size;
    }
    return PyUnicode_FromFormat("<builtin awaitable at %p>",
                                self);
}

static PyObject *
awaitable_send_with_arg(PyObject *self, PyObject *value)
{
    PyAwaitableObject *aw = (PyAwaitableObject *) self;
    if (aw->aw_gen == NULL) {
        PyObject* gen = awaitable_next(self);
        if (gen == NULL)
            return NULL;

        Py_RETURN_NONE;
    }

    return gen_next(aw->aw_gen);
}

static PyObject *
awaitable_send(PyObject *self, PyObject *args)
{
    PyObject *value;

    if (!PyArg_ParseTuple(args, "O", &value))
        return NULL;

    return awaitable_send_with_arg(self, value);
}

static PyObject *
awaitable_close(PyObject *self, PyObject *args)
{
    PyAwaitable_Cancel(self);
    PyAwaitableObject *aw = (PyAwaitableObject *) self;
    aw->aw_done = true;
    Py_RETURN_NONE;
}

static PyObject *
awaitable_throw(PyObject *self, PyObject *args)
{
    PyObject *type;
    PyObject *value = NULL;
    PyObject *traceback = NULL;

    if (!PyArg_ParseTuple(args, "O|OO", &type, &value, &traceback))
        return NULL;

    if (PyType_Check(type)) {
        PyObject *err = PyObject_Vectorcall(type, (PyObject*[]) { value }, 1, NULL);
        if (err == NULL) {
            return NULL;
        }

        if (traceback)
            if (PyException_SetTraceback(err, traceback) < 0)
            {
                Py_DECREF(err);
                return NULL;
            }

        PyErr_SetRaisedException(err);
    } else
        PyErr_SetRaisedException(Py_NewRef(type));

    PyAwaitableObject *aw = (PyAwaitableObject *) self;
    if ((aw->aw_gen != NULL) && (aw->aw_state != 0)) {
        GenWrapperObject *gw = (GenWrapperObject *) aw->aw_gen;
        awaitable_callback* cb = aw->aw_callbacks[aw->aw_state - 1];
        if (cb == NULL)
            return NULL;

        if (fire_err_callback(self, gw->gw_current_await, cb) < 0)
            return NULL;
    } else
        return NULL;

    assert(NULL);
}

static PySendResult
awaitable_am_send(PyObject *self, PyObject *arg, PyObject **presult) {
    PyObject *send_res = awaitable_send_with_arg(self, arg);
    if (send_res == NULL) {
        PyObject *occurred = PyErr_GetRaisedException();
        if (PyErr_GivenExceptionMatches(occurred, PyExc_StopIteration)) {
            PyObject *item = PyObject_GetAttrString(occurred, "value");

            if (item == NULL) {
                return PYGEN_ERROR;
            }

            *presult = item;
            return PYGEN_RETURN;
        }
        PyErr_SetRaisedException(occurred);
        *presult = NULL;
        return PYGEN_ERROR;
    }
    *presult = send_res;

    return PYGEN_NEXT;
}

static PyMethodDef awaitable_methods[] = {
    {"send", awaitable_send, METH_VARARGS, NULL},
    {"close", awaitable_close, METH_VARARGS, NULL},
    {"throw", awaitable_throw, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyAsyncMethods async_methods = {
    .am_await = awaitable_next,
    .am_send = awaitable_am_send
};

PyTypeObject _PyAwaitable_GenWrapper_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_GenWrapper", 
    sizeof(GenWrapperObject),
    0,
    gen_dealloc,                                /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    PyDoc_STR(""),                              /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    gen_next,                                   /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    gen_new,                                    /* tp_new */
};

PyTypeObject PyAwaitable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "awaitable",
    sizeof(PyAwaitableObject),
    0,
    awaitable_dealloc,                          /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    &async_methods,                             /* tp_as_async */
    awaitable_repr,                             /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    awaitable_doc,                              /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    awaitable_next,                             /* tp_iternext */
    awaitable_methods,                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    awaitable_new,                              /* tp_new */
};

void
PyAwaitable_Cancel(PyObject *aw)
{
    assert(aw != NULL);
    Py_INCREF(aw);

    PyAwaitableObject *a = (PyAwaitableObject *) aw;

    for (int i = 0; i < a->aw_callback_size; i++) {
        awaitable_callback* cb = a->aw_callbacks[i];
        if (!cb->done)
            Py_DECREF(cb->coro);
    }

    PyMem_Free(a->aw_callbacks);
    a->aw_callback_size = 0;
    Py_DECREF(aw);
}

int
PyAwaitable_AddAwait(
    PyObject *aw,
    PyObject *coro,
    awaitcallback cb,
    awaitcallback_err err
)
{
    assert(aw != NULL);
    assert(coro != NULL);
    Py_INCREF(coro);
    Py_INCREF(aw);
    PyAwaitableObject *a = (PyAwaitableObject *) aw;

    awaitable_callback *aw_c = malloc(sizeof(awaitable_callback));
    if (aw_c == NULL) {
        Py_DECREF(aw);
        Py_DECREF(coro);
        PyErr_NoMemory();
        return -1;
    }

    ++a->aw_callback_size;
    if (a->aw_callbacks == NULL) {
        a->aw_callbacks = PyMem_Calloc(a->aw_callback_size,
        sizeof(awaitable_callback *));
    } else {
        a->aw_callbacks = PyMem_Realloc(a->aw_callbacks,
        sizeof(awaitable_callback *) * a->aw_callback_size
    );
    }

    if (a->aw_callbacks == NULL) {
        --a->aw_callback_size;
        Py_DECREF(aw);
        Py_DECREF(coro);
        free(aw_c);
        PyErr_NoMemory();
        return -1;
    }

    aw_c->coro = coro; // steal our own reference
    aw_c->callback = cb;
    aw_c->err_callback = err;
    a->aw_callbacks[a->aw_callback_size - 1] = aw_c;
    Py_DECREF(aw);

    return 0;
}

int
PyAwaitable_SetResult(PyObject *awaitable, PyObject *result)
{
    assert(awaitable != NULL);
    assert(result != NULL);
    Py_INCREF(result);
    Py_INCREF(awaitable);

    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;
    if (aw->aw_gen == NULL) {
        PyErr_SetString(PyExc_TypeError, "no generator is currently present");
        Py_DECREF(awaitable);
        Py_DECREF(result);
        return -1;
    }
    _PyAwaitable_GenWrapper_SetResult(aw->aw_gen, result);
    Py_DECREF(awaitable);
    Py_DECREF(result);
    return 0;
}

int
PyAwaitable_UnpackValuesVa(PyObject *awaitable, ...) {
    assert(awaitable != NULL);
    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;
    Py_INCREF(awaitable);

    if (aw->aw_values == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "awaitable object has no stored values");
        Py_DECREF(awaitable);
        return -1;
    }

    va_list args;
    va_start(args, awaitable);

    for (int i = 0; i < aw->aw_values_size; i++) {
        PyObject **ptr = va_arg(args, PyObject **);
        if (ptr == NULL) continue;
        *ptr = aw->aw_values[i];
        // borrowed reference
    }

    va_end(args);
    Py_DECREF(awaitable);
    return 0;
}

int
PyAwaitable_UnpackValues(PyObject *awaitable, PyObject **values) {
    assert(awaitable != NULL);
    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;
    Py_INCREF(awaitable);

    if (aw->aw_values == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "awaitable object has no stored values");
        Py_DECREF(awaitable);
        return -1;
    }

    for (int i = 0; i < aw->aw_values_size; i++) {
        values[i] = aw->aw_values[i];
        // borrowed reference
    }

    Py_DECREF(awaitable);
    return 0;
}

int
PyAwaitable_SaveValuesVa(PyObject *awaitable, Py_ssize_t nargs, ...) {
    assert(awaitable != NULL);
    assert(nargs != 0);
    Py_INCREF(awaitable);
    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;

    va_list vargs;
    va_start(vargs, nargs);
    int offset = aw->aw_values_size;

    if (aw->aw_values == NULL)
        aw->aw_values = PyMem_Calloc(
            nargs,
            sizeof(PyObject *)
        );
    else
        aw->aw_values = PyMem_Realloc(
            aw->aw_values,
            sizeof(PyObject *) * (aw->aw_values_size + nargs)
        );

    if (aw->aw_values == NULL) {
        PyErr_NoMemory();
        Py_DECREF(awaitable);
        return -1;
    }

    aw->aw_values_size += nargs;

    for (int i = offset; i < aw->aw_values_size; i++)
        aw->aw_values[i] = Py_NewRef(va_arg(vargs, PyObject*));

    va_end(vargs);
    Py_DECREF(awaitable);
    return 0;
}

int
PyAwaitable_SaveValues(PyObject *awaitable, Py_ssize_t nargs, PyObject **values) {
    assert(awaitable != NULL);
    assert(nargs != 0);
    Py_INCREF(awaitable);
    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;

    int offset = aw->aw_values_size;

    if (aw->aw_values == NULL)
        aw->aw_values = PyMem_Calloc(
            nargs,
            sizeof(PyObject *)
        );
    else
        aw->aw_values = PyMem_Realloc(
            aw->aw_values,
            sizeof(PyObject *) * (aw->aw_values_size + nargs)
        );

    if (aw->aw_values == NULL) {
        PyErr_NoMemory();
        Py_DECREF(awaitable);
        return -1;
    }

    aw->aw_values_size += nargs;

    for (int i = offset; i < aw->aw_values_size; i++)
        aw->aw_values[i] = Py_NewRef(values[i]);

    Py_DECREF(awaitable);
    return 0;
}

int
PyAwaitable_UnpackArbValues(PyObject *awaitable, void **values) {
    assert(awaitable != NULL);
    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;
    Py_INCREF(awaitable);

    if (aw->aw_values == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "awaitable object has no stored arbitrary values");
        Py_DECREF(awaitable);
        return -1;
    }


    for (int i = 0; i < aw->aw_arb_values_size; i++) {
        values[i] = aw->aw_arb_values[i];
    }

    Py_DECREF(awaitable);
    return 0;
}


int
PyAwaitable_UnpackArbValuesVa(PyObject *awaitable, ...) {
    assert(awaitable != NULL);
    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;
    Py_INCREF(awaitable);

    if (aw->aw_values == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "awaitable object has no stored arbitrary values");
        Py_DECREF(awaitable);
        return -1;
    }

    va_list args;
    va_start(args, awaitable);

    for (int i = 0; i < aw->aw_arb_values_size; i++) {
        void **ptr = va_arg(args, void **);
        if (ptr == NULL) continue;
        *ptr = aw->aw_arb_values[i];
    }

    va_end(args);
    Py_DECREF(awaitable);
    return 0;
}

int
PyAwaitable_SaveArbValuesVa(PyObject *awaitable, Py_ssize_t nargs, ...) {
    assert(awaitable != NULL);
    assert(nargs != 0);
    Py_INCREF(awaitable);
    PyAwaitableObject *aw = (PyAwaitableObject *) awaitable;

    va_list vargs;
    va_start(vargs, nargs);
    int offset = aw->aw_arb_values_size;

    if (aw->aw_arb_values == NULL)
        aw->aw_arb_values = PyMem_Calloc(
            nargs,
            sizeof(void *)
        );
    else
        aw->aw_arb_values = PyMem_Realloc(
            aw->aw_arb_values,
            sizeof(void *) * (aw->aw_arb_values_size + nargs)
        );

    if (aw->aw_arb_values == NULL) {
        PyErr_NoMemory();
        Py_DECREF(awaitable);
        return -1;
    }

    aw->aw_arb_values_size += nargs;

    for (int i = offset; i < aw->aw_arb_values_size; i++)
        aw->aw_arb_values[i] = va_arg(vargs, void *);

    va_end(vargs);
    Py_DECREF(awaitable);
    return 0;
}

PyObject *
PyAwaitable_New(void)
{
    PyObject *aw = awaitable_new(&PyAwaitable_Type, NULL, NULL);
    return aw;
}