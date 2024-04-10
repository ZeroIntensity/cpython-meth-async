import asyncio
import _meth_async as ma

async def foo():
    await asyncio.sleep(1)
    print("awaited by C!")

asyncio.run(ma.test_func(foo()))