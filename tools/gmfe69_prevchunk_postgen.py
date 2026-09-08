#!/usr/bin/env python3
"""Compatibility-safe extra GMFE69 burst postgen.

Adds a one-entry previous-chunk range cache to chassis_dispatch_burst after the
normal gmfe69_postgen has already produced its validated direct-dispatch form.
This intentionally does not edit gmfe69_postgen.py itself so it can coexist
with local renderer/PS3 work in that file.
"""

from __future__ import annotations

from pathlib import Path

MARK = "MOH_GMFE69_BURST_PREVIOUS_RANGE_FASTPATH"


def apply_gmfe69_prevchunk_postgen(path: Path) -> None:
    path = Path(path)
    text = path.read_text()

    if MARK in text:
        return

    burst_start = text.find("static u32 chassis_dispatch_burst(")
    if burst_start < 0:
        raise RuntimeError("GMFE69 prevchunk postgen: chassis_dispatch_burst not found")
    state_anchor = "static void chassis_on_state_loaded(CPUState* ctx)\n"
    burst_end = text.find(state_anchor, burst_start)
    if burst_end < 0:
        raise RuntimeError("GMFE69 prevchunk postgen: burst end anchor not found")

    burst = text[burst_start:burst_end]

    # Require the already-validated direct chunk dispatcher.  This avoids
    # accidentally touching an older/legacy burst implementation.
    if "MultiChunkDispatch cached_chunk_dispatch = 0;" not in burst:
        raise RuntimeError(
            "GMFE69 prevchunk postgen: direct chunk dispatcher is not present"
        )
    if "MOH_GMFE69_BURST_ADJACENT_CHUNK_FASTPATH" not in burst:
        raise RuntimeError(
            "GMFE69 prevchunk postgen: adjacent-chunk fastpath must run first"
        )

    decl = """    MultiImageDispatch cached_dispatch = 0;\n    MultiChunkDispatch cached_chunk_dispatch = 0;\n"""
    decl_repl = """    MultiImageDispatch cached_dispatch = 0;\n    MultiChunkDispatch cached_chunk_dispatch = 0;\n    /* MOH_GMFE69_BURST_PREVIOUS_RANGE_FASTPATH\n     * One-entry MRU range cache for caller <-> callee ping-pong.  The existing\n     * safety/variant/dispatch validation still runs after a range hit; only the\n     * address -> canonical-chunk lookup is skipped. */\n    int previous_chunk = -1;\n    u32 previous_chunk_start = 0u;\n    u32 previous_chunk_end = 0u;\n"""
    if burst.count(decl) != 1:
        raise RuntimeError(
            "GMFE69 prevchunk postgen: expected one direct cache declaration block"
        )
    burst = burst.replace(decl, decl_repl, 1)

    # The normal postgen has already replaced the original multi_chunk_index
    # call with the adjacent next/prev resolver.  Seed that resolver from the
    # previous validated range first.  If it misses, behavior is byte-for-byte
    # the existing adjacent/page-hint path.
    resolver = """            int resolved_chunk = -1;\n\n            /* MOH_GMFE69_BURST_ADJACENT_CHUNK_FASTPATH */\n"""
    resolver_repl = """            int resolved_chunk = -1;\n\n            if (previous_chunk >= 0 &&\n                pc >= previous_chunk_start && pc < previous_chunk_end)\n                resolved_chunk = previous_chunk;\n\n            /* MOH_GMFE69_BURST_ADJACENT_CHUNK_FASTPATH */\n"""
    if burst.count(resolver) != 1:
        raise RuntimeError(
            "GMFE69 prevchunk postgen: expected one adjacent resolver anchor"
        )
    burst = burst.replace(resolver, resolver_repl, 1)

    # Do not let the adjacent resolver overwrite an MRU hit.
    adjacent_open = """            if (chunk >= 0) {\n"""
    adjacent_open_repl = """            if (resolved_chunk < 0 && chunk >= 0) {\n"""
    # Restrict this replacement to the resolver region: only the first one
    # after our marker is relevant.
    marker_pos = burst.find(MARK)
    resolver_pos = burst.find("int resolved_chunk = -1;", marker_pos)
    pos = burst.find(adjacent_open, resolver_pos)
    if pos < 0:
        raise RuntimeError("GMFE69 prevchunk postgen: adjacent resolver body not found")
    burst = burst[:pos] + adjacent_open_repl + burst[pos + len(adjacent_open):]

    # Before promoting a newly resolved chunk, remember the current validated
    # range.  We cache only index/range, not dispatch state: all original burst
    # guards remain in force, making this substantially safer than Gather24-like
    # batching or cross-block state elision.
    promote = """            chunk = resolved_chunk;\n            if (chunk < 0) break;\n            cached_chunk = chunk;\n            cached_chunk_start = s_chunk_ranges[(u32)chunk].start;\n            cached_chunk_end = s_chunk_ranges[(u32)chunk].end;\n"""
    promote_repl = """            chunk = resolved_chunk;\n            if (chunk < 0) break;\n            if (cached_chunk >= 0 && chunk != cached_chunk) {\n                previous_chunk = cached_chunk;\n                previous_chunk_start = cached_chunk_start;\n                previous_chunk_end = cached_chunk_end;\n            }\n            cached_chunk = chunk;\n            cached_chunk_start = s_chunk_ranges[(u32)chunk].start;\n            cached_chunk_end = s_chunk_ranges[(u32)chunk].end;\n"""
    if burst.count(promote) != 1:
        raise RuntimeError(
            "GMFE69 prevchunk postgen: expected one chunk promotion block"
        )
    burst = burst.replace(promote, promote_repl, 1)

    # Integrity checks: preserve the current safety path and direct dispatcher.
    required = (
        MARK,
        "MOH_GMFE69_BURST_ADJACENT_CHUNK_FASTPATH",
        "MOH_GMFE69_BURST_GUARD_HOIST",
        "cached_chunk_dispatch(ctx);",
        "if (resolved_chunk < 0)\n                resolved_chunk = multi_chunk_index(pc);",
        "chain_state[chunk_u] == 0u",
        "end - begin != 1u",
    )
    for needle in required:
        if needle not in burst:
            raise RuntimeError(
                f"GMFE69 prevchunk postgen: verification marker missing: {needle}"
            )

    text = text[:burst_start] + burst + text[burst_end:]
    path.write_text(text)
    print("  GMFE69 burst previous-range cache: enabled (safe lookup-only MRU)")


__all__ = ("apply_gmfe69_prevchunk_postgen",)
