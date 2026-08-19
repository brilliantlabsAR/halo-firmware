import argparse
import cv2
import numpy as np
from serial import Serial
from tqdm import tqdm  # Import tqdm for progress tracking

def parse_args():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(description='Display BGGR8 RAW from serial')
    parser.add_argument('--port', required=True, help='COM port name (e.g., COM3)')
    parser.add_argument('--baudrate', type=int, default=115200, help='Baud rate')
    return parser.parse_args()

def find_sync(buffer):
    """Find the position of the sync header (0x55AA55AA)."""
    sync = b'\xAA\x55\xAA\x55'  # Little-endian format
    return buffer.find(sync)

def process_frame(buffer, progress_bar):
    """Process a complete frame of image data."""
    if len(buffer) < 8:
        return None, buffer  # Not enough data, wait for more

    # Skip the sync header (4 bytes)
    header = buffer[4:8]
    width = int.from_bytes(header[0:2], byteorder='little')
    height = int.from_bytes(header[2:4], byteorder='little')

    # Calculate the required data length
    expected_len = 8 + width * height  # 8 bytes (header + sync) + image data
    progress_bar.total = expected_len  # Update progress bar total
    progress_bar.n = len(buffer)  # Update progress bar current value
    progress_bar.refresh()  # Refresh the progress bar
    if len(buffer) < expected_len:
        return None, buffer  # Not enough data, wait for more

    # Extract image data
    raw_data = np.frombuffer(buffer[8:expected_len], dtype=np.uint8)

    # Save raw data to a file with a timestamped filename
    import time
    timestamp = int(time.time())
    with open(f"raw_data_{timestamp}.raw", "wb") as f:
        f.write(raw_data)
    
    # Convert to a 2D array
    bayer_image = raw_data.reshape((height, width))
    
    # Demosaicing (BGGR format)
    rgb_image = cv2.demosaicing(bayer_image, cv2.COLOR_BayerBG2RGB)
    
    return rgb_image, buffer[expected_len:]

def show(ser):
    """Continuously read data from the serial port and display the image."""
    buffer = bytearray()
    progress_bar = tqdm(total=100, desc="Receiving Data", unit="%")  # Initialize progress bar

    while True:
        # Read serial data
        data = ser.read(ser.in_waiting or 1)
        if data:
            buffer.extend(data)

        # Search for the sync header
        while True:
            sync_pos = find_sync(buffer)
            if sync_pos == -1:
                # If no sync header is found, retain the last 3 bytes to avoid losing partial headers
                if len(buffer) > 4:
                    buffer = buffer[-3:]
                break

            # If sync header is found, discard preceding invalid data
            if sync_pos > 0:
                buffer = buffer[sync_pos:]

            # Attempt to process a frame
            frame, remaining = process_frame(buffer, progress_bar)
            if frame is not None:
                # Display the image
                cv2.imshow('BGGR Preview', frame)
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    progress_bar.close()
                    return
                buffer = remaining
            else:
                break  # Wait for more data

if __name__ == '__main__':
    args = parse_args()

    try:
        with Serial(args.port, args.baudrate, timeout=1) as ser:
            print(f"Connected to {ser.port} @ {ser.baudrate}bps")
            show(ser)
    except Exception as e:
        print(f"Error: {str(e)}")
    finally:
        cv2.destroyAllWindows()
