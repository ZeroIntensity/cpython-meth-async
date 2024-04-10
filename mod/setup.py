from setuptools import Extension, setup

if __name__ == '__main__':
    setup(
        name="meth_async",
        ext_modules=[Extension("_meth_async", ["./mod.c"], include_dirs=["../Include/"])],
        license="MIT",
    )
