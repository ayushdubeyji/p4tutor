import serial
import time
import sys

try:
    ser = serial.Serial('COM6', 115200, timeout=1)
    # Reset board via DTR/RTS
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    
    start_time = time.time()
    while time.time() - start_time < 5.0:
        line = ser.readline()
        if line:
            print(line.decode('utf-8', errors='ignore').strip())
            
    ser.close()
except Exception as e:
    print(f"Error: {e}")
