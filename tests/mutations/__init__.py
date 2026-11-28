"""The mutations tests/mutation_check.py applies, one module per area.

Each entry breaks one behaviour and names the tests that should notice:

    name       unique; --only matches on it
    file       the file to change, from the repository root
    find/into  the text to replace, and what replaces it
    count      how many times find must match (default 1)
    caught_by  the ctest names that must all fail
    requires   a fixture without which the tests cannot see the change
"""

from . import cpu, io, memory, vera, ym

MUTATIONS = cpu.MUTATIONS + memory.MUTATIONS + io.MUTATIONS + vera.MUTATIONS + ym.MUTATIONS
