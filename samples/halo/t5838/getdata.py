import argparse
import serial
import wave
import serial.tools.list_ports
from playsound import playsound
import sys
import datetime
import os

def list_serial_ports():
    # List available serial ports
    ports = list(serial.tools.list_ports.comports())
    for i, port in enumerate(ports):
        print(f"{i}: {port.device} - {port.description}")
    return ports

def select_serial_port():
    # Prompt user to select a serial port
    ports = list_serial_ports()
    if not ports:
        print("No serial ports found.")
        sys.exit(1)
    idx = int(input("Select the serial port number: "))
    return ports[idx].device

def get_data_from_serial(port_name, baudrate, output_raw):
    # Receive serial data and save to temporary raw file
    ser = serial.Serial(port_name, baudrate, timeout=5)
    ser.flushInput()

    recording = False
    buffer = b''

    with open(output_raw, 'a') as f:
        while True:
            count = ser.in_waiting
            if count:
                data = ser.read(count)
                buffer += data

                if b'Start Speaking or Play some Audio' in buffer:
                    print(">>> Recording started...")
                    buffer = b''

                elif b'Stop recording' in buffer:
                    recording = True
                    print(">>> Recording stopped. Fetching data...")
                    buffer = b''

                elif b'PDM example, print done' in buffer:
                    print(">>> Process complete.")
                    break

                elif recording:
                    try:
                        f.write(data.decode('utf-8'))
                    except UnicodeDecodeError:
                        pass

def pcm_to_wav(pcm_file, wav_file, channels=1, bits=16, sample_rate=16000):
    # Convert hex string raw data to WAV format
    last_pcm_data = '00' if bits == 8 else '0000'
    count = 0

    with open(pcm_file, 'r') as pcmf:
        pcm_data_len = len(pcmf.read())
        pcmf.seek(0)

        with wave.open(wav_file, 'wb') as wavfile:
            wavfile.setnchannels(channels)
            wavfile.setsampwidth(bits // 8)
            wavfile.setframerate(sample_rate)
            
            while count < pcm_data_len:
                line = pcmf.readline()
                line_len = len(line)
                stripped = line.strip()

                if len(stripped) == 0:
                    count += line_len
                    continue

                if (bits == 8 and len(stripped) == 2) or (bits == 16 and len(stripped) == 4):
                    last_pcm_data = stripped
                    value = int(last_pcm_data, 16)
                    
                else:
                    value = int(last_pcm_data, 16)

                if bits == 8:
                    # 8-bit WAV uses signed format (-128~127)
                    if value > 127:
                        value -= 128
                    else:
                        value += 128
                    #print(value)
                    wavfile.writeframes(value.to_bytes(1, byteorder='little'))
                elif bits == 16:
                    # 16-bit WAV uses signed format (-32768~32767)
                    wavfile.writeframes(value.to_bytes(2, byteorder='little'))

                count += line_len

def main():
    # Argument parser setup
    parser = argparse.ArgumentParser(description="Record PCM data via serial and convert to WAV.")
    parser.add_argument('--port', type=str, help='Serial port (e.g. /dev/ttyUSB0 or COM3)')
    parser.add_argument('--baudrate', type=int, default=115200, help='Baudrate (default: 115200)')
    parser.add_argument('--channels', type=int, default=2, help='Number of audio channels (default: 1)')
    parser.add_argument('--bits', type=int, default=16, help='Bit depth (8 or 16, default: 16)')
    parser.add_argument('--rate', type=int, default=8000, help='Sample rate in Hz (default: 8000)')
    parser.add_argument('--output', type=str, help='Output WAV filename (will be auto-named if not provided)')
    parser.add_argument('--play', action='store_true', help='Play WAV file after generation')
    args = parser.parse_args()

    # Generate filenames based on timestamp if not provided
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    wav_file = args.output or f"record_{timestamp}.wav"
    pcm_file = f"record_{timestamp}.raw"

    # Remove previous temp file if exists
    if os.path.exists(pcm_file):
        os.remove(pcm_file)

    # Select serial port
    port = args.port or select_serial_port()

    # Record and convert
    get_data_from_serial(port, args.baudrate, pcm_file)
    pcm_to_wav(pcm_file, wav_file, args.channels, args.bits, args.rate)

    # Optionally play the result
    if args.play:
        input("Press Enter to play the audio...")
        playsound(wav_file)

    print(f"WAV file saved as: {wav_file}")
    print("Done.")

if __name__ == '__main__':
    main()
