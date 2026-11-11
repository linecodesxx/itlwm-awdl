#!/usr/bin/env python3
"""Read-only, bounded direct-call xrefs from local Tahoe 25G332 KC.

Reports symbolic callers of three addresses without emitting disassembly.
Cannot detect virtual, indirect or inlined calls, so absence is NOT proof.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

DEFAULT_KC = '/System/Library/KernelCollections/BootKernelExtensions.kc'
DEFAULT_WTL = os.path.expanduser('~/Desktop/WTLKcAbi')
OBJDUMP_FALLBACK = ('/Applications/Xcode.app/Contents/Developer/Toolchains/'
                    'XcodeDefault.xctoolchain/usr/bin/llvm-objdump')
TARGETS = {
    0xffffff8000acaf80: 'IOWorkLoop::addEventSource',
    0xffffff8000acb070: 'IOWorkLoop::removeEventSource',
    0xffffff8002b31d68: 'IOSkywalkTxSubmissionQueue::packetSubmission(bool)',
}
BRANCH = re.compile(r'\b(call|jmp)\s+(0x[0-9a-fA-F]+)\b')
INDIRECT = re.compile(r'\b(call|jmp)\s+qword ptr\s+\[([a-z0-9]+)\s*\+\s*(0x[0-9a-fA-F]+)\]')
PRIORITY = ('register', 'deregister', 'attach', 'detach', 'initialize',
            'initWith', 'start', 'stop', 'enable', 'disable', 'finalize',
            'free', 'workLoop', 'queue', 'packet')


def demangle_many(symbols):
    output = {}
    for start in range(0, len(symbols), 100):
        group = symbols[start:start+100]
        p = subprocess.run(['c++filt'] + group, capture_output=True, text=True)
        if p.returncode or len(p.stdout.splitlines()) != len(group):
            raise RuntimeError('c++filt failed or truncated output')
        output.update(zip(group, p.stdout.splitlines()))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kc', default=DEFAULT_KC)
    parser.add_argument('--wtlkcabi', default=DEFAULT_WTL)
    parser.add_argument('--families', default='IOSkywalk,IOUserNetwork,IOEthernet')
    parser.add_argument('--max-functions', type=int, default=180)
    parser.add_argument('--skip-functions', type=int, default=0,
                        help='Skip this many ranked unique functions before scanning')
    parser.add_argument('--max-lines', type=int, default=400)
    parser.add_argument('--virtual-offsets', default='',
                        help='Comma-separated indirect call slot offsets to flag (candidate only)')
    parser.add_argument('--max-virtual-hits', type=int, default=80)
    parser.add_argument('--out', default='/tmp/awdl-skywalk-direct-callers.txt')
    args = parser.parse_args()
    if args.skip_functions < 0:
        parser.error('--skip-functions must be nonnegative')
    if args.max_functions <= 0 or args.max_lines <= 0 or args.max_virtual_hits <= 0:
        parser.error('limits must be positive')
    if not os.path.isfile(args.kc) or not os.path.isdir(args.wtlkcabi):
        parser.error('local KC or WTLKcAbi directory missing')
    try:
        virtual_offsets = set(int(x.strip(), 0) for x in
                              args.virtual_offsets.split(',') if x.strip())
    except ValueError:
        parser.error('invalid --virtual-offsets value')
    if any(x < 0 or x % 8 for x in virtual_offsets):
        parser.error('--virtual-offsets must be nonnegative 8-byte multiples')
    prefixes = tuple(p.strip() for p in args.families.split(',') if p.strip())
    if not prefixes:
        parser.error('provide at least one family prefix')
    sys.path.insert(0, args.wtlkcabi)
    from kcfile import KC
    kc = KC(args.kc)
    mangled = sorted(sym for sym in kc.byname
                     if sym.startswith('__ZN')
                     and any(prefix in sym for prefix in prefixes))
    decoded = demangle_many(mangled)
    selected = []
    for sym, readable in decoded.items():
        if not readable.startswith(prefixes) or '::' not in readable:
            continue
        try:
            addr = kc.find(sym)
        except (KeyError, ValueError):
            continue
        if not isinstance(addr, int) or not addr:
            continue
        method = readable.split('::', 1)[1]
        rank = next((i for i, term in enumerate(PRIORITY)
                     if term in method), len(PRIORITY))
        selected.append((rank, readable, addr))
    selected.sort(key=lambda x: (x[0], x[1], x[2]))
    deduped, seen_addresses = [], set()
    for item in selected:
        if item[2] not in seen_addresses:
            deduped.append(item)
            seen_addresses.add(item[2])
    start_index = args.skip_functions
    selected = deduped[start_index:start_index + args.max_functions]

    if not os.environ.get('LLVM_OBJDUMP') and os.path.isfile(OBJDUMP_FALLBACK):
        os.environ['LLVM_OBJDUMP'] = OBJDUMP_FALLBACK
    from disasm import Disassembler
    hits, indirect_hits, errors, truncated = [], [], 0, 0
    with tempfile.TemporaryDirectory(prefix='itlwm-awdl-calls-') as workdir:
        dis = Disassembler(kc, workdir)
        for _, readable, address in selected:
            try:
                lines = dis.function(address) or []
            except (RuntimeError, ValueError, KeyError, OSError):
                errors += 1
                continue
            if len(lines) > args.max_lines:
                truncated += 1
            for line in lines[:args.max_lines]:
                if virtual_offsets:
                    indirect = INDIRECT.search(line)
                    if indirect and int(indirect.group(3), 16) in virtual_offsets:
                        indirect_hits.append((readable, '%#x' % address,
                                              line.split(':', 1)[0].strip(),
                                              indirect.group(1), indirect.group(2),
                                              int(indirect.group(3), 16)))
                found = BRANCH.search(line)
                if not found:
                    continue
                target = TARGETS.get(int(found.group(2), 16))
                if target:
                    site = line.split(':', 1)[0].strip()
                    hits.append((readable, '%#x' % address, site,
                                 found.group(1), target))
    with open(args.out, 'w', encoding='utf-8') as out:
        out.write('Read-only direct-call target scan from local KC\n')
        out.write('KC: %s\n' % args.kc)
        out.write('Families: %s\n' % ', '.join(prefixes))
        out.write('Scanned functions: %d / %d matched symbols\n' %
                  (len(selected), len(deduped)))
        out.write('Selection range (0-based): [%d, %d) of %d\n' %
                  (start_index, start_index + len(selected), len(deduped)))
        out.write('Disassembly failures: %d; bodies truncated: %d\n' %
                  (errors, truncated))
        out.write('Targets: %s\n' % ', '.join('%s=%#x' % (name, addr)
                   for addr, name in TARGETS.items()))
        out.write('Direct call/jmp hits: %d\n\n' % len(hits))
        for caller, address, site, kind, target in hits:
            out.write('%s (%s): %s at %s -> %s\n' %
                      (caller, address, kind, site, target))
        if not hits:
            out.write('No direct calls found in this BOUNDED selection.\n')
        if virtual_offsets:
            out.write('\nCANDIDATE indirect slot calls: %d total; showing up to %d\n' %
                      (len(indirect_hits), args.max_virtual_hits))
            out.write('Requested slot offsets: %s\n' %
                      ', '.join('%#x' % x for x in sorted(virtual_offsets)))
            for caller, address, site, kind, register, offset in indirect_hits[:args.max_virtual_hits]:
                out.write('%s (%s): %s at %s via [%s + %#x]\n' %
                          (caller, address, kind, site, register, offset))
            if len(indirect_hits) > args.max_virtual_hits:
                out.write('... %d more candidates omitted by report limit\n' %
                          (len(indirect_hits) - args.max_virtual_hits))
            out.write('CAUTION: an indirect memory call matching an offset does NOT\n'
                      'prove receiver class, vtable usage, or target method.\n')
        out.write('\nLIMITATION: misses indirect/virtual calls, other symbol '
                  'families, functions outside scan limit, and calls '
                  'beyond max-lines. No-hit is NOT a safety proof.\n')
        out.write('No proprietary disassembly is included in this report.\n')
    print('Saved interpreted call-site summary:', args.out)
    print('Scanned:', len(selected), 'of', len(deduped), 'symbolic functions; range:',
          start_index, '-', start_index + len(selected), '; direct hits:',
          len(hits), '; candidate indirect-slot hits:', len(indirect_hits))
    return 0


if __name__ == '__main__':
    sys.exit(main())
