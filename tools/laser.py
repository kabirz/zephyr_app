import serial
import argparse
import threading
import time

parser = argparse.ArgumentParser("laser emul")
parser.add_argument('device', help="uart com")
parser.add_argument('-b', '--bps', default=115200, help="uart bps")

args = parser.parse_args()

ser = serial.Serial(
    port=args.device,
    baudrate=args.bps,
    bytesize=serial.SEVENBITS,
    parity=serial.PARITY_EVEN,
    stopbits=serial.STOPBITS_ONE
)

send_data_enable = False
thread_running = False
tt = 100
id = 0
def send_data(serial: serial.Serial):
    val = 0
    while thread_running:
        if send_data_enable:
            val = (val+100) % (1<<24)
            serial.write(f'g{id}h+{val}\r\n'.encode())
            time.sleep(tt/1000)
        else:
            time.sleep(1)

thread = threading.Thread(target=send_data, args=(ser,))
thread_running = True
thread.start()

try:
    while True:
        msg = ser.readline().decode()
        if msg[0] == 's':
            id = msg[1]
            if msg[2] == 'c' and msg[3] =='\r': # clear
                print('clear')
                send_data_enable = False
                ser.write(b'g0?\r\n')
            elif msg[2] == 'o':
                print('on')
                ser.write(b'g0?\r\n')
            elif msg[2:4] == 're' : # read error
                print('read error')
                ser.write(b'g0re+200+300\r\n')
            elif msg[2:4] == 'ce' : # clear error
                print('clear error')
                ser.write(b'g0ce?\r\n')
            elif msg[2] == 'h':
                if msg[3] == '+':
                    tt = int(msg[3:])
                send_data_enable = True
                print(f'start measure, period: {tt}')
            else:
                print(f"error: {msg}")
        else:
            print(f"error: {msg}")
except KeyboardInterrupt:
    print('close serial')
    thread_running = False
    thread.join()
    ser.close()
