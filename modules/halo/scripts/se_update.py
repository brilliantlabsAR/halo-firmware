#!/usr/bin/env python3
"""
SE Firmware Update Example using mcumgr

This script demonstrates how to update SE firmware over BLE using the
SE management group (Group ID: 64).

Requirements:
- mcumgr tool installed
- SE firmware file (pupd.bin or fupd.bin)
- Device in BLE DFU mode

Usage:
    python se_update.py <device_address> <firmware_file> [version] [type]

Example:
    python se_update.py F1:23:45:67:89:AB pupd.bin "1.108.0" "pupd"
"""

import sys
import subprocess
import json

def run_mcumgr_command(cmd):
    """Run mcumgr command and return result"""
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)

def get_se_version(device_addr):
    """Get current SE version"""
    cmd = f"mcumgr --conntype ble --connstring peer={device_addr} se version"
    ret, stdout, stderr = run_mcumgr_command(cmd)

    if ret == 0:
        try:
            response = json.loads(stdout)
            return response.get('version', 'unknown')
        except:
            return 'unknown'
    else:
        print(f"Failed to get SE version: {stderr}")
        return None

def update_se_firmware(device_addr, firmware_file, target_version=None, update_type="pupd"):
    """Update SE firmware"""
    # Build the command
    cmd = f"mcumgr --conntype ble --connstring peer={device_addr} se update"

    # Add parameters
    params = []
    params.append(f"image@{firmware_file}")

    if target_version:
        params.append(f"version={target_version}")

    params.append(f"type={update_type}")

    cmd += " " + " ".join(params)

    print(f"Running command: {cmd}")
    ret, stdout, stderr = run_mcumgr_command(cmd)

    if ret == 0:
        try:
            response = json.loads(stdout)
            status = response.get('status', 'unknown')
            message = response.get('message', '')

            if status == 'success':
                print("SE firmware update successful!")
                if message:
                    print(f"Message: {message}")
                return True
            elif status == 'already_up_to_date':
                print("SE firmware is already up to date.")
                return True
            else:
                print(f"Update failed with status: {status}")
                return False
        except json.JSONDecodeError:
            print(f"Invalid JSON response: {stdout}")
            return False
    else:
        print(f"Command failed: {stderr}")
        return False

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    device_addr = sys.argv[1]
    firmware_file = sys.argv[2]
    target_version = sys.argv[3] if len(sys.argv) > 3 else None
    update_type = sys.argv[4] if len(sys.argv) > 4 else "pupd"

    print(f"SE Firmware Update Tool")
    print(f"Device: {device_addr}")
    print(f"Firmware: {firmware_file}")
    print(f"Target Version: {target_version or 'any'}")
    print(f"Update Type: {update_type}")
    print()

    # Check if firmware file exists
    import os
    if not os.path.exists(firmware_file):
        print(f"Error: Firmware file '{firmware_file}' not found!")
        sys.exit(1)

    # Get current SE version
    print("Checking current SE version...")
    current_version = get_se_version(device_addr)
    if current_version:
        print(f"Current SE version: {current_version}")
    else:
        print("Warning: Could not retrieve current SE version")

    print()

    # Perform update
    print("Starting SE firmware update...")
    success = update_se_firmware(device_addr, firmware_file, target_version, update_type)

    if success:
        print("\nSE firmware update completed successfully!")
        print("Device should reboot automatically to apply the update.")
    else:
        print("\nSE firmware update failed!")
        sys.exit(1)

if __name__ == "__main__":
    main()