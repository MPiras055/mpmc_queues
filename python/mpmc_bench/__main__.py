"""`python -m mpmc_bench` -> the experiment runner."""

import sys

from .runner import main

if __name__ == "__main__":
    sys.exit(main())
