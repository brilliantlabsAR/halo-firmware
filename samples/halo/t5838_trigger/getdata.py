import argparse
import serial
import wave
import serial.tools.list_ports
from playsound import playsound
import sys
import datetime
import os

def list_serial_ports():
    ports = list(serial.tools.list_ports.comports())
    for i, port in enumerate(ports):
        print(f"{i}: {port.device} - {port.description}")
    return ports

def select_serial_port():
    ports = list_serial_ports()
    if not ports:
        print("No serial ports found.")
        sys.exit(1)
    idx = int(input("Select the serial port number: "))
    return ports[idx].device

def get_data_from_serial(port_name, baudrate, output_raw):
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
    last_pcm_data = '0000'
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
                if len(line.strip()) == 4:
                    last_pcm_data = line.strip()
                    value = int(last_pcm_data, 16)
                else:
                    value = int(last_pcm_data, 16)

                wavfile.writeframes(value.to_bytes(2, byteorder='little'))
                count += line_len

def main():
    parser = argparse.ArgumentParser(description="Record PCM data via serial and convert to WAV.")
    parser.add_argument('--port', type=str, help='Serial port (e.g. /dev/ttyUSB0 or COM3)')
    parser.add_argument('--baudrate', type=int, default=115200, help='Baudrate (default: 115200)')
    parser.add_argument('--channels', type=int, default=2, help='Number of audio channels (default: 2)')
    parser.add_argument('--bits', type=int, default=16, help='Bit depth (default: 16)')
    parser.add_argument('--rate', type=int, default=8000, help='Sample rate in Hz (default: 8000)')
    parser.add_argument('--output', type=str, help='Output WAV filename (will be auto-named if not provided)')
    parser.add_argument('--play', action='store_true', help='Play WAV file after generation')
    args = parser.parse_args()

    # Generate filename based on time if not provided
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    wav_file = args.output or f"record_{timestamp}.wav"
    pcm_file = f"record_{timestamp}.raw"

    # Select port
    port = args.port or select_serial_port()

    # Clear previous temp file
    if os.path.exists(pcm_file):
        os.remove(pcm_file)

    # Run
    get_data_from_serial(port, args.baudrate, pcm_file)
    pcm_to_wav(pcm_file, wav_file, channels=args.channels, bits=args.bits, sample_rate=args.rate)

    if args.play:
        input("Press Enter to play the audio...")
        playsound(wav_file)

    print(f"WAV file saved as: {wav_file}")
    print("Done.")

if __name__ == '__main__':
    main()
