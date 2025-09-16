#!/usr/bin/python3
"""
USB Bus Reset Utility for Elo TouchSystems Devices.

Sometimes, when we change monitor arrangement, the Elo screens will get the new
position but xinput will not see them as a mouse (touchscreen -> regular screen).

This module provides functionality to reset USB buses containing Elo TouchSystems
devices by toggling their authorization status in the Linux sysfs filesystem,
and the OS will reinitialize them and re-read them as a "mouse".
"""

import os
import sys
import logging
import concurrent.futures
from pathlib import Path
from typing import Set, Optional
from dataclasses import dataclass
from contextlib import contextmanager

try:
    import usb.core
    import usb.backend.libusb1
except ImportError:
    print("Error: pyusb library not installed.", file=sys.stderr)
    sys.exit(1)

# Constants
ELO_VENDOR_ID = 0x04E7     # Elo TouchSystems vendor ID
DEFAULT_RESET_DELAY = 1.0  # Delay in seconds between disable and enable
SYSFS_USB_PATH = Path("/sys/bus/usb/devices")

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


@dataclass
class USBBusInfo:
    """Container for USB bus information."""
    bus_number: int
    device_count: int
    sysfs_path: Path


class USBBusResetError(Exception):
    """Custom exception for USB bus reset operations."""
    pass


@contextmanager
def usb_backend():
    """
    Context manager for USB backend operations.
    
    Yields:
        usb.backend.libusb1.Backend: LibUSB backend instance
        
    Note:
        Ensures proper cleanup of USB resources.
    """
    backend = usb.backend.libusb1.get_backend()
    try:
        yield backend
    finally:
        # Cleanup is handled automatically by pyusb
        pass


def find_elo_buses(vendor_id: int = ELO_VENDOR_ID) -> Set[USBBusInfo]:
    """
    Find all USB buses containing devices from the specified vendor.
    
    Args:
        vendor_id: USB vendor ID to search for (default: Elo TouchSystems)
        
    Returns:
        Set of USBBusInfo objects for buses containing matching devices
        
    Raises:
        USBBusResetError: If USB device enumeration fails
    """
    buses = {}
    
    try:
        with usb_backend():
            devices = usb.core.find(find_all=True, idVendor=vendor_id)
            
            for device in devices:
                bus_num = device.bus
                if bus_num not in buses:
                    sysfs_path = SYSFS_USB_PATH / f"usb{bus_num}" / "authorized"
                    
                    # Verify the sysfs path exists
                    if not sysfs_path.exists():
                        logger.warning(f"sysfs path not found for bus {bus_num}: {sysfs_path}")
                        continue
                        
                    buses[bus_num] = USBBusInfo(
                        bus_number=bus_num,
                        device_count=1,
                        sysfs_path=sysfs_path
                    )
                else:
                    buses[bus_num].device_count += 1
                    
    except Exception as e:
        raise USBBusResetError(f"Failed to enumerate USB devices: {e}")
    
    return set(buses.values())


def reset_usb_bus(bus_info: USBBusInfo, reset_delay: float = DEFAULT_RESET_DELAY) -> bool:
    """
    Reset a single USB bus by toggling its authorization status.
    
    Args:
        bus_info: USBBusInfo object containing bus details
        reset_delay: Delay in seconds between disable and enable operations
        
    Returns:
        True if reset was successful, False otherwise
        
    Note:
        This function performs the actual sysfs write operations to disable
        and re-enable the USB bus.
    """
    try:
        # Disable the bus
        logger.info(f"Disabling USB bus {bus_info.bus_number} ({bus_info.device_count} Elo device(s))")
        bus_info.sysfs_path.write_text("0\n")
        
        # Wait for the specified delay
        import time
        time.sleep(reset_delay)
        
        # Re-enable the bus
        logger.info(f"Re-enabling USB bus {bus_info.bus_number}")
        bus_info.sysfs_path.write_text("1\n")
        
        logger.info(f"Successfully reset USB bus {bus_info.bus_number}")
        return True
        
    except PermissionError:
        logger.error(f"Permission denied for bus {bus_info.bus_number}. Run with sudo.")
        return False
    except Exception as e:
        logger.error(f"Failed to reset bus {bus_info.bus_number}: {e}")
        return False


def reset_buses_parallel(buses: Set[USBBusInfo], reset_delay: float = DEFAULT_RESET_DELAY) -> int:
    """
    Reset multiple USB buses in parallel using thread pool.
    
    Args:
        buses: Set of USBBusInfo objects to reset
        reset_delay: Delay in seconds between disable and enable operations
        
    Returns:
        Number of successfully reset buses
        
    Note:
        Uses concurrent.futures for efficient parallel execution.
        All buses are disabled simultaneously, then re-enabled after the delay.
    """
    if not buses:
        logger.warning("No buses to reset")
        return 0
    
    logger.info(f"Resetting {len(buses)} USB bus(es) simultaneously")
    
    # Use ThreadPoolExecutor for I/O-bound operations
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(buses)) as executor:
        # Phase 1: Disable all buses simultaneously
        disable_futures = []
        for bus in buses:
            future = executor.submit(
                lambda b: b.sysfs_path.write_text("0\n"),
                bus
            )
            disable_futures.append((future, bus))
        
        # Wait for all disable operations to complete
        for future, bus in disable_futures:
            try:
                future.result(timeout=1.0)
                logger.info(f"Disabled bus {bus.bus_number}")
            except Exception as e:
                logger.error(f"Failed to disable bus {bus.bus_number}: {e}")
        
        # Wait for the reset delay
        import time
        time.sleep(reset_delay)
        
        # Phase 2: Re-enable all buses simultaneously
        enable_futures = []
        for bus in buses:
            future = executor.submit(
                lambda b: b.sysfs_path.write_text("1\n"),
                bus
            )
            enable_futures.append((future, bus))
        
        # Count successful operations
        success_count = 0
        for future, bus in enable_futures:
            try:
                future.result(timeout=1.0)
                logger.info(f"Re-enabled bus {bus.bus_number}")
                success_count += 1
            except Exception as e:
                logger.error(f"Failed to re-enable bus {bus.bus_number}: {e}")
    
    return success_count


def check_permissions() -> bool:
    """
    Check if the script has sufficient permissions to modify USB bus states.
    
    Returns:
        True if running with root privileges, False otherwise
        
    Note:
        On Linux, modifying sysfs USB attributes requires root access.
    """
    return os.geteuid() == 0


def main(reset_delay: Optional[float] = None, vendor_id: Optional[int] = None) -> int:
    """
    Main entry point for the USB bus reset utility.
    
    Args:
        reset_delay: Optional custom delay between disable/enable (seconds)
        vendor_id: Optional custom USB vendor ID to search for
        
    Returns:
        Exit code (0 for success, non-zero for errors)
    """
    # Check permissions first
    if not check_permissions():
        logger.error("Insufficient permissions. This script must be run with sudo.")
        return 1
    
    # Use provided parameters or defaults
    delay = reset_delay or DEFAULT_RESET_DELAY
    vid = vendor_id or ELO_VENDOR_ID
    
    try:
        # Find buses with Elo devices
        buses = find_elo_buses(vendor_id=vid)
        
        if not buses:
            logger.info(f"No USB buses found with devices from vendor 0x{vid:04X}")
            return 0
        
        # Log discovered buses
        total_devices = sum(bus.device_count for bus in buses)
        logger.info(
            f"Found {total_devices} Elo device(s) across {len(buses)} bus(es): "
            f"{', '.join(str(bus.bus_number) for bus in sorted(buses, key=lambda b: b.bus_number))}"
        )
        
        # Reset buses in parallel
        success_count = reset_buses_parallel(buses, reset_delay=delay)
        
        if success_count == len(buses):
            logger.info(f"Successfully reset all {success_count} bus(es)")
            return 0
        elif success_count > 0:
            logger.warning(f"Partially successful: reset {success_count}/{len(buses)} bus(es)")
            return 2
        else:
            logger.error("Failed to reset any buses")
            return 1
            
    except USBBusResetError as e:
        logger.error(f"USB reset error: {e}")
        return 1
    except KeyboardInterrupt:
        logger.info("Operation cancelled by user")
        return 130  # Standard exit code for SIGINT
    except Exception as e:
        logger.error(f"Unexpected error: {e}")
        return 1


if __name__ == "__main__":
    # Parse command line arguments if needed
    import argparse
    
    parser = argparse.ArgumentParser(
        description="Reset USB buses containing Elo TouchSystems devices"
    )
    parser.add_argument(
        "-d", "--delay",
        type=float,
        default=DEFAULT_RESET_DELAY,
        help=f"Delay in seconds between disable and enable (default: {DEFAULT_RESET_DELAY})"
    )
    parser.add_argument(
        "-v", "--vendor",
        type=lambda x: int(x, 0),  # Support hex (0x...) and decimal
        default=ELO_VENDOR_ID,
        help=f"USB vendor ID to search for (default: 0x{ELO_VENDOR_ID:04X})"
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Enable debug logging"
    )
    
    args = parser.parse_args()
    
    if args.debug:
        logging.getLogger().setLevel(logging.DEBUG)
    
    sys.exit(main(reset_delay=args.delay, vendor_id=args.vendor))
