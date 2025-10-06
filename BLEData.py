from bleak import BleakClient

ADDR = "XX:XX:XX:XX:XX:XX"
CHAR_UUID = "88664422-3412-7856-1234-785612341235"

def handle_notify(_, data):
    print("RMS =", int.from_bytes(data, 'little') / 100.0)

async def main():
    async with BleakClient(ADDR) as client:
        await client.start_notify(CHAR_UUID, handle_notify)
        await asyncio.sleep(10)

import asyncio
asyncio.run(main())

