#!/usr/bin/env python3
"""
Memory Monitor Tool

This tool monitors the memory usage of a process over time and outputs the results to a CSV file.
It tracks Virtual Memory (VM) and Resident Set Size (RSS) at specified intervals.

Usage:
    python3 memory_monitor.py [OPTIONS] <granularity> <max_rows> <outfile> <command...>

Arguments:
    granularity    Sampling interval in seconds (float)
    max_rows       Maximum number of samples to collect
    outfile        Output CSV file path
    command        Command and arguments to monitor

Options:
    --unit UNIT    Memory unit (10=KB, 20=MB, 30=GB, default: 20)
    --batch-size N Batch size for CSV writes (default: 1000)
    --no-convert   Skip memory unit conversion
    --verbose      Enable verbose output
    --help         Show this help message
"""

import argparse
import csv
import subprocess
import sys
import time
import signal
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
import logging


@dataclass
class MemoryInfo:
    """Represents memory information for a process"""
    vm_pages: int
    rss_pages: int
    step: int
    timestamp: float

    def to_bytes(self, page_size: int) -> tuple[int, int]:
        """Convert pages to bytes"""
        return self.vm_pages * page_size, self.rss_pages * page_size

    def to_list(self) -> list[int]:
        """Convert to list format for CSV writing"""
        return [self.vm_pages, self.rss_pages, self.step]


class MemoryMonitor:
    """Main class for monitoring process memory usage"""

    def __init__(self, granularity: float, max_rows: int, outfile: str,
                     batch_size: int = 1000, verbose: bool = False):
            self.granularity: float = granularity
            self.max_rows: int = max_rows
            self.outfile: Path = Path(outfile)
            self.batch_size: int = batch_size
            self.verbose: bool = verbose
            self.page_size: int = self._get_page_size()
            self.process: subprocess.Popen[bytes] | None = None

            # Setup logging
            logging.basicConfig(
                level=logging.INFO if verbose else logging.WARNING,
                format='%(asctime)s - %(levelname)s - %(message)s'
            )
            self.logger: logging.Logger = logging.getLogger(__name__)

            # Validate parameters
            self._validate_parameters()

    def _validate_parameters(self) -> None:
        """Validate input parameters"""
        if self.granularity <= 0:
            raise ValueError("Granularity must be a positive float")

        if self.max_rows <= 0:
            raise ValueError("max_rows must be a positive integer")

        if self.batch_size <= 0:
            raise ValueError("batch_size must be a positive integer")

        # Check if output directory exists
        if not self.outfile.parent.exists():
            raise ValueError(f"Output directory does not exist: {self.outfile.parent}")

    @staticmethod
    def _get_page_size() -> int:
        """Get system page size in bytes"""
        try:
            result = subprocess.run(
                ["getconf", "PAGESIZE"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5
            )
            if result.returncode == 0:
                return int(result.stdout.strip())
        except (subprocess.TimeoutExpired, subprocess.SubprocessError, ValueError) as e:
            logging.warning(f"Could not get page size via getconf: {e}")

        # Fallback to common page size
        return 4096

    def _get_memory_info(self, pid: int) -> MemoryInfo|None:
        """
        Get memory information for a process

        Args:
            pid: Process ID

        Returns:
            MemoryInfo object or None if failed
        """
        statm_file = Path(f"/proc/{pid}/statm")

        try:
            with statm_file.open('r') as file:
                line = file.readline().strip()
                if not line:
                    return None

                parts = line.split()
                if len(parts) < 2:
                    return None

                vm_pages = int(parts[0])
                rss_pages = int(parts[1])

                return MemoryInfo(
                    vm_pages=vm_pages,
                    rss_pages=rss_pages,
                    step=0,  # Will be set by caller
                    timestamp=time.time()
                )

        except (OSError, IOError, ValueError, IndexError) as e:
            self.logger.warning(f"Error reading memory info for PID {pid}: {e}")
            return None

    @contextmanager
    def _signal_handler(self):
        """Context manager to handle process termination gracefully"""
        def signal_handler(signum, frame):
            self.logger.info("Received signal, terminating monitored process...")
            if self.process:
                self.process.terminate()
                try:
                    _ = self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.logger.warning("Process didn't terminate gracefully, killing...")
                    self.process.kill()
            sys.exit(0)

        old_handlers = {}
        for sig in [signal.SIGINT, signal.SIGTERM]:
            old_handlers[sig] = signal.signal(sig , signal_handler)

        try:
            yield
        finally:
            # Restore original signal handlers
            for sig, handler in old_handlers.items():
                signal.signal(sig, handler)

    def monitor(self, command: list[str]) -> None:
        """
        Monitor memory usage of a command

        Args:
            command: Command and arguments to execute and monitor
        """
        self.logger.info(f"Starting memory monitoring for command: {' '.join(command)}")
        self.logger.info(f"Output file: {self.outfile}")
        self.logger.info(f"Granularity: {self.granularity}s, Max rows: {self.max_rows}")

        with self._signal_handler():
            # Start the process
            try:
                self.process = subprocess.Popen(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE
                )
            except (OSError, subprocess.SubprocessError) as e:
                raise RuntimeError(f"Failed to start process: {e}")

            pid = self.process.pid
            self.logger.info(f"Started process with PID: {pid}")

            # Monitor memory usage
            self._monitor_process(pid)

    def _monitor_process(self, pid: int) -> None:
        """Monitor memory usage of a specific process"""
        header = ["VM", "RSS", "Step"]
        step = 0
        row_count = 0
        memory_batch: list[MemoryInfo] = []
        if self.process is None:
            return None

        try:
            with self.outfile.open("w", newline='') as csvfile:
                writer = csv.writer(csvfile)
                writer.writerow(header)

                while row_count < self.max_rows:
                    # Check if process is still running
                    if self.process.poll() is not None:
                        self.logger.info("Process has terminated")
                        break

                    # Get memory information
                    mem_info = self._get_memory_info(pid)
                    if mem_info is None:
                        self.logger.warning(f"Could not read memory info for PID {pid}, process may have ended")
                        break

                    mem_info.step = step
                    memory_batch.append(mem_info)

                    step += 1
                    row_count += 1

                    # Write batch when full
                    if len(memory_batch) >= self.batch_size:
                        self._write_batch(writer, memory_batch)
                        memory_batch.clear()

                    # Check if we've reached max rows
                    if row_count >= self.max_rows:
                        self.logger.info(f"Reached maximum rows ({self.max_rows}), terminating process")
                        self.process.terminate()
                        break

                    time.sleep(self.granularity)

                # Write remaining data
                if memory_batch:
                    self._write_batch(writer, memory_batch)

        except KeyboardInterrupt:
            self.logger.info("Monitoring interrupted by user")
        except Exception as e:
            self.logger.error(f"Error during monitoring: {e}")
            raise
        finally:
            # Ensure process is cleaned up
            if self.process and self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()

        # Get final return code
        return_code = self.process.wait() if self.process else 0
        if return_code != 0:
            self.logger.warning(f"Process exited with return code: {return_code}")

        self.logger.info(f"Monitoring completed. Collected {row_count} samples.")

    def _write_batch(self, writer, memory_batch: list[MemoryInfo]) -> None:
        """Write a batch of memory data to CSV"""
        rows = [mem_info.to_list() for mem_info in memory_batch]
        writer.writerows(rows)

    def convert_units(self, unit_shift: int = 20) -> None:
        """
        Convert memory units in the CSV file

        Args:
            unit_shift: Bit shift for unit conversion (10=KB, 20=MB, 30=GB)
        """
        if unit_shift < 0:
            raise ValueError("Unit shift must be non-negative")

        unit_names = {10: "KB", 20: "MB", 30: "GB", 0: "bytes"}
        unit_name = unit_names.get(unit_shift, f"2^{unit_shift} bytes")

        self.logger.info(f"Converting memory units to {unit_name}")

        # Use a more robust temporary file approach
        temp_file = self.outfile.with_suffix('.tmp')

        try:
            with self.outfile.open('r') as infile, temp_file.open('w', newline='') as outfile:
                reader = csv.reader(infile)
                writer = csv.writer(outfile)

                # Copy header
                try:
                    header = next(reader)
                    writer.writerow(header)
                except StopIteration:
                    raise ValueError("Input CSV file is empty")

                # Process data in batches
                batch = []
                for row in reader:
                    if len(row) < 2:
                        self.logger.warning(f"Skipping malformed row: {row}")
                        continue

                    try:
                        # Convert VM and RSS from pages to specified unit
                        vm_bytes = int(row[0]) * self.page_size
                        rss_bytes = int(row[1]) * self.page_size

                        row[0] = round(vm_bytes / (2 ** unit_shift), 2)
                        row[1] = round(rss_bytes / (2 ** unit_shift), 2)

                        batch.append(row)

                        if len(batch) >= self.batch_size:
                            writer.writerows(batch)
                            batch.clear()

                    except (ValueError, IndexError) as e:
                        self.logger.warning(f"Skipping invalid row {row}: {e}")
                        continue

                # Write remaining rows
                if batch:
                    writer.writerows(batch)

            # Replace original file with converted file
            temp_file.replace(self.outfile)
            self.logger.info("Unit conversion completed")

        except Exception as e:
            # Clean up temporary file on error
            if temp_file.exists():
                temp_file.unlink()
            raise RuntimeError(f"Failed to convert units: {e}")


def create_argument_parser() -> argparse.ArgumentParser:
    """Create and configure argument parser"""
    parser = argparse.ArgumentParser(
        description="Monitor process memory usage over time",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Monitor for 10 seconds with 0.1s intervals, max 1000 samples
  %(prog)s 0.1 1000 memory.csv python3 -c "import time; time.sleep(10)"

  # Monitor with verbose output and convert to MB
  %(prog)s --verbose --unit 20 0.5 2000 memory.csv ./my_program

  # Monitor without unit conversion
  %(prog)s --no-convert 1.0 500 memory.csv make -j4
        """
    )

    parser.add_argument("granularity", type=float,
                       help="Sampling interval in seconds")
    parser.add_argument("max_rows", type=int,
                       help="Maximum number of samples to collect")
    parser.add_argument("outfile", type=str,
                       help="Output CSV file path")
    parser.add_argument("command", nargs="+",
                       help="Command and arguments to monitor")

    parser.add_argument("--unit", type=int, default=20, choices=[0, 10, 20, 30],
                       help="Memory unit: 0=bytes, 10=KB, 20=MB, 30=GB (default: 20)")
    parser.add_argument("--batch-size", type=int, default=1000,
                       help="Batch size for CSV writes (default: 1000)")
    parser.add_argument("--no-convert", action="store_true",
                       help="Skip memory unit conversion (keep in pages)")
    parser.add_argument("--verbose", action="store_true",
                       help="Enable verbose output")

    return parser


def main() -> int:
    """Main function"""
    parser = create_argument_parser()
    args = parser.parse_args()

    try:
        # Create monitor instance
        monitor = MemoryMonitor(
            granularity=args.granularity,
            max_rows=args.max_rows,
            outfile=args.outfile,
            batch_size=args.batch_size,
            verbose=args.verbose
        )

        # Start monitoring
        monitor.monitor(args.command)

        # Convert units if requested
        if not args.no_convert:
            monitor.convert_units(args.unit)

        return 0

    except KeyboardInterrupt:
        print("\nOperation cancelled by user", file=sys.stderr)
        return 130
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
