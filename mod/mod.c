#include <Python.h>

static PyObject *
test_func(PyObject *self, PyObject *awaitable, PyObject *args) {
    PyObject *coro;
    if (!PyArg_ParseTuple(args, "O", &coro))
        return NULL;

    if (PyAwaitable_AWAIT(awaitable, coro) < 0)
        return NULL;

    return PyLong_FromLong(42);
}

static PyMethodDef methods[] = {
    {"test_func", test_func, METH_VARARGS | METH_ASYNC, NULL},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_meth_async",
    NULL,
    -1,
    methods
};

PyMODINIT_FUNC PyInit__meth_async(void) {
    return PyModule_Create(&module);
}