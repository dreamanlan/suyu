# command script import /Users/dreaman/Documents/GitHub/myuzu/tools/lldb_bpdata.py
# script lldb_bpdata.print_threads_stacks("")
# target stop-hook add -P lldb_bpdata.MyStopHook

# watchpoint set expression -w write -s 8 -- (0xb400007262a9a0b0+0xc8)
# memory read -s 1 -c 0x20000 -o e:\tlsf_crash_mem.txt --force 0x00000071813c0000

# breakpoint set -f VKDescriptorState.cpp -l 247 -C 'p/x *(long*)(*(long*)((char*)m_UpdateTemplate+0x10)+8)' -C 'var m_Bindings->buffer' -C 'c'
# breakpoint set -y VKDescriptorState.cpp:247 -C 'p/x *(long*)(*(long*)((char*)m_UpdateTemplate+0x10)+8)' -C 'var m_Bindings->buffer' -C 'c'
# breakpoint set -C 'var e' -C 'c' -y GpuProgramsVK.cpp:622

import lldb
import binascii
import struct
import os

_fmt_map = {
    "u8": ("B", 1), "i8": ("b", 1),
    "u16": ("H", 2), "i16": ("h", 2),
    "u32": ("I", 4), "i32": ("i", 4),
    "u64": ("Q", 8), "i64": ("q", 8),
    "f32": ("f", 4), "f64": ("d", 8)
}

hook_module = True
file_path = '/Users/dreaman/Documents/databp.txt'
file_object = None

def del_file():
	global file_path
	if os.path.exists(file_path):
		os.remove(file_path)

def open_file():
	global file_object
	file_object = open(file_path, 'a')

def flush_file():
	global file_object
	if file_object is not None:
		file_object.flush()

def close_file():
	global file_object
	if file_object is not None:
		file_object.close()
	file_object = None

def write_to_file(content):
	global file_object
	if file_object is None:
		open_file()
	if file_object is not None:
		file_object.write(content)
	else:
		print("Error: File is not open")

def get_regs(frame, kind):
	for reg_set in frame.regs:
		if kind.lower() in str(reg_set.name).lower():
			return reg_set
	return None

def get_GPRs(frame):
	return get_regs(frame, 'general purpose')

def get_FPRs(frame):
	return get_regs(frame, 'floating point')

def get_ESRs(frame):
	return get_regs(frame, 'exception state')

def print_modules(key):
	debugger = lldb.debugger
	target = debugger.GetSelectedTarget()
	for module in target.module_iter():
		if key in str(module.file.GetFilename()).lower():
			addr = module.GetObjectFileHeaderAddress()
			entry = module.GetObjectFileEntryPointAddress()
			print("Module Addr:load 0x{:016x} file 0x{:016x} Entry:load 0x{:016x} file 0x{:016x} File:{}".format(addr.load_addr, addr.file_addr, entry.load_addr, entry.file_addr, module))
			for section in module.section_iter():
				print("  Section: ", section)

def print_threads(key):
	debugger = lldb.debugger
	process = debugger.GetSelectedTarget().GetProcess()
	for thread in process:
		if key in str(thread.name).lower():
			print(thread)

def print_breakpoints(key):
	debugger = lldb.debugger
	target = debugger.GetSelectedTarget()
	for breakpoint in target.breakpoint_iter():
		for location in breakpoint:
			symbol = location.GetAddress().GetSymbol()
			if symbol.IsValid() and key in str(symbol.GetName()).lower():
				print(breakpoint)

def print_threads_stacks(key):
	dbg = lldb.debugger
	result = lldb.SBCommandReturnObject();
	ci = dbg.GetCommandInterpreter()
	debugger = lldb.debugger
	process = debugger.GetSelectedTarget().GetProcess()
	for thread in process:
		if key in str(thread.name).lower():
			print(thread)
			for f in thread.frames:
				print(f)

def find_symbol_instances(target, name):
    found = False
    for mi in range(target.GetNumModules()):
        mod = target.GetModuleAtIndex(mi)
        syms = mod.FindSymbols(name)
        if syms.GetSize() == 0:
            continue
        for si in range(syms.GetSize()):
            found = True
            sc = syms.GetContextAtIndex(si)
            sym = sc.GetSymbol()
            if not sym.IsValid():
                continue
            sname = sym.GetName() or "<no-name>"
            stype = sym.GetType()
            bsize = sym.GetSize()
            start_addr = sym.GetStartAddress()
            la = start_addr.GetLoadAddress(target)
            sect_name = "<no-section>"
            if la != lldb.LLDB_INVALID_ADDRESS:
                sect = target.ResolveLoadAddress(la).GetSection()
                if sect.IsValid():
                    sect_name = "%s" % (sect.GetName())
            print("---- module[%d] %s ----" % (mi, mod.GetFileSpec().GetFilename()))
            print("  symbol name: %s" % sname)
            print("  symbol type(enum): %d  byte_size: %d" % (stype, bsize))
            print("  load_addr: 0x%x  section: %s" % (la if la != lldb.LLDB_INVALID_ADDRESS else 0, sect_name))
            process = target.GetProcess()
            if la != lldb.LLDB_INVALID_ADDRESS and bsize and process.IsValid():
                err = lldb.SBError()
                data = process.ReadMemory(la, bsize, err)
                if err.Success():
                    hx = binascii.hexlify(data).decode('ascii')
                    grouped = ' '.join([hx[i:i+2] for i in range(0, len(hx), 2)])
                    print("  memory (%d bytes): %s" % (bsize, grouped))
                else:
                    print("  ReadMemory failed: %s" % err.GetCString())
            else:
                print("  cannot read memory (invalid addr or size)")
            print("")
    if not found:
        print("No symbol named '%s' found in any module." % name)

def read_variable_index_values(target, varname, index, offsets_sizes, endian="<"):
    """
    Find all symbol instances named `varname` in `target`, pick the instance at `index`
    (zero-based across all found instances), read memory at each specified offset, and
    return a list of values only.

    Return list semantics (same order as offsets_sizes):
      - If fmt is provided and parse succeeds -> parsed value (int or float)
      - If fmt is None and read succeeds -> raw bytes (Python bytes)
      - On read or parse error -> None

    Parameters:
      - target: SBTarget
      - varname: symbol name (string)
      - index: zero-based index selecting which found symbol instance to use
      - offsets_sizes: list of tuples (offset:int, size:int, fmt:None or fmt string)
            fmt can be None or one of "u8/i8/u16/.../f32/f64"
      - endian: "<" for little-endian (default) or ">" for big-endian
    """
    # collect all symbol contexts that match varname across modules
    contexts = []
    num_modules = target.GetNumModules()
    for mi in range(num_modules):
        mod = target.GetModuleAtIndex(mi)
        syms = mod.FindSymbols(varname)
        if syms.GetSize() == 0:
            continue
        for si in range(syms.GetSize()):
            sc = syms.GetContextAtIndex(si)
            sym = sc.GetSymbol()
            if sym.IsValid():
                contexts.append((mod, sym))

    # check index
    if index < 0 or index >= len(contexts):
        raise IndexError("symbol index out of range: %d (found %d instances)" % (index, len(contexts)))

    mod, sym = contexts[index]
    process = target.GetProcess()

    # get load address
    start_addr = sym.GetStartAddress()
    la = start_addr.GetLoadAddress(target)
    if la == lldb.LLDB_INVALID_ADDRESS:
        # cannot read; return list of None for each requested offset
        return [None for _ in offsets_sizes]

    results = []
    for (offset, sz, fmt) in offsets_sizes:
        read_addr = la + int(offset)
        # attempt to read
        if not process.IsValid():
            results.append(None)
            continue
        err = lldb.SBError()
        data = process.ReadMemory(read_addr, int(sz), err)
        if not err.Success():
            results.append(None)
            continue

        if fmt is None:
            results.append(data)
            continue

        fmt_l = fmt.lower()
        if fmt_l not in _fmt_map:
            results.append(None)
            continue
        fmt_char, expected_size = _fmt_map[fmt_l]
        if expected_size != int(sz):
            results.append(None)
            continue
        struct_fmt = endian + fmt_char
        try:
            val = struct.unpack(struct_fmt, data)[0]
            results.append(val)
        except Exception:
            results.append(None)

    return results

# Example usage (in LLDB Python environment):
# target = lldb.debugger.GetSelectedTarget()
# values = read_variable_index_values(target, "my_var", 0, [(0,4,"u32"), (8,8,"f64"), (16,4,None)])
# # values might be like: [1234, 3.14, b'\x01\x02\x03\x04']

def write_variable_index_values(target, varname, index, writes, endian="<"):
    """
    Find all symbol instances named `varname` in `target`, pick the instance at `index`
    (zero-based across all found instances), and write specified values into memory
    at offsets relative to the symbol's load address.

    Parameters:
      - target: SBTarget
      - varname: symbol name (string)
      - index: zero-based index selecting which found symbol instance to use
      - writes: list of tuples (offset:int, size:int, fmt:None or fmt string, value)
            * If fmt is None: `value` should be bytes-like (bytes or bytearray) of length == size.
            * If fmt is provided (e.g. "u32", "f64"): `value` should be an int or float
              that can be packed with struct according to the fmt. The packed size must equal size.
      - endian: "<" for little-endian (default) or ">" for big-endian

    Return:
      - A list of booleans (same order as `writes`), True if the individual write succeeded,
        False if it failed (read/pack/write errors etc).

    Notes:
      - Raises IndexError if index is out of range (no such symbol instance).
      - Does not raise on per-field errors; those yield False in the returned list.
    """
    # collect all symbol contexts that match varname across modules
    contexts = []
    num_modules = target.GetNumModules()
    for mi in range(num_modules):
        mod = target.GetModuleAtIndex(mi)
        syms = mod.FindSymbols(varname)
        if syms.GetSize() == 0:
            continue
        for si in range(syms.GetSize()):
            sc = syms.GetContextAtIndex(si)
            sym = sc.GetSymbol()
            if sym.IsValid():
                contexts.append((mod, sym))

    # check index
    if index < 0 or index >= len(contexts):
        raise IndexError("symbol index out of range: %d (found %d instances)" % (index, len(contexts)))

    _, sym = contexts[index]
    process = target.GetProcess()

    # get load address
    start_addr = sym.GetStartAddress()
    la = start_addr.GetLoadAddress(target)
    if la == lldb.LLDB_INVALID_ADDRESS:
        # cannot write; return list of False for each requested write
        return [False for _ in writes]

    results = []
    for (offset, sz, fmt, value) in writes:
        write_addr = la + int(offset)

        # process validity
        if not process.IsValid():
            results.append(False)
            continue

        # prepare bytes to write
        data = None
        if fmt is None:
            # expect bytes-like
            if isinstance(value, (bytes, bytearray)):
                if len(value) != int(sz):
                    results.append(False)
                    continue
                data = bytes(value)
            else:
                # reject non-bytes
                results.append(False)
                continue
        else:
            fmt_l = fmt.lower()
            if fmt_l not in _fmt_map:
                results.append(False)
                continue
            fmt_char, expected_size = _fmt_map[fmt_l]
            if expected_size != int(sz):
                results.append(False)
                continue
            struct_fmt = endian + fmt_char
            try:
                # allow passing bytes directly if exactly right length
                if isinstance(value, (bytes, bytearray)):
                    if len(value) != int(sz):
                        results.append(False)
                        continue
                    data = bytes(value)
                else:
                    # pack numeric value
                    data = struct.pack(struct_fmt, value)
            except Exception:
                results.append(False)
                continue

        # attempt to write memory
        err = lldb.SBError()
        written = process.WriteMemory(write_addr, data, err)
        if not err.Success():
            results.append(False)
            continue
        # some implementations return -1 or 0 on failure; require full length written
        if written != len(data):
            results.append(False)
            continue

        results.append(True)

    return results

# Example usage (in LLDB Python environment):
# target = lldb.debugger.GetSelectedTarget()
# writes = [
#     (0, 4, "u32", 1234),         # write 4-byte unsigned int 1234 at offset 0
#     (8, 8, "f64", 3.14159),      # write double at offset 8
#     (16, 4, None, b'\x01\x02\x03\x04')  # write raw 4 bytes at offset 16
# ]
# success_flags = write_variable_index_values(target, "my_var", 0, writes)
# # success_flags might be like: [True, True, True]

def start():
	close_file()
	del_file()
	debugger = lldb.debugger
	target = debugger.GetSelectedTarget()
	for module in target.module_iter():
		if "yuzu" in str(module.file.GetFilename()).lower():
			addr = module.GetObjectFileHeaderAddress()
			entry = module.GetObjectFileEntryPointAddress()
			minfo = "Module Addr:load 0x{:016x} file 0x{:016x} Entry:load 0x{:016x} file 0x{:016x} File:{}".format(addr.load_addr, addr.file_addr, entry.load_addr, entry.file_addr, module)
			write_to_file(minfo)
			write_to_file('\n')
			print(minfo)
			for section in module.section_iter():
				sinfo = "  Section: {}".format(section)
				write_to_file(sinfo)
				write_to_file('\n')
				print(sinfo)
	process = target.GetProcess()
	debugger.HandleCommand("target stop-hook delete")
	debugger.HandleCommand("target stop-hook add -P lldb_bpdata.MyStopHook")
	process.Continue()

def resume():
	debugger = lldb.debugger
	target = debugger.GetSelectedTarget()
	process = target.GetProcess()
	debugger.HandleCommand("target stop-hook delete")
	debugger.HandleCommand("target stop-hook add -P lldb_bpdata.MyStopHook")
	process.Continue()

class MyStopHook(object):

	def __init__(self, target, extra_args, internal_dict):
		self.target = target

	def handle_stop(self, exe_ctx, stream):
		global hook_module

		target = self.target
		frame = exe_ctx.frame
		thread = frame.GetThread()
		process = thread.GetProcess()
		tinfo = str(thread)
		write_to_file(tinfo)
		write_to_file('\n')
		stream.Print(tinfo)
		stream.Print('\n')

		if thread.stop_reason == lldb.eStopReasonWatchpoint:
			if len(thread.frames) > 0:
				gprs = get_GPRs(thread.frames[0])
				if gprs is not None:
					for reg in gprs:
						if reg.name[0] != 'w':
							reg_info = "  {}: {}".format(reg.GetName(), reg.GetValue())
							write_to_file(reg_info)
							write_to_file('\n')
							stream.Print(reg_info)
							stream.Print('\n')

			for f in thread.frames:
				finfo = str(f)
				write_to_file(finfo)
				write_to_file('\n')
				stream.Print(finfo)
				stream.Print('\n')

		if hook_module:
			find_symbol_instances(target, "g_WatchPointCommandInfoVK")
		else:
			find_symbol_instances(target, "g_WatchPointCommandInfo")

		if hook_module:
			cmd, flag, size, addr, tid = read_variable_index_values(target, "g_WatchPointCommandInfoVK", 0, [(0,2,'i16'),(2,2,'i16'),(4,4,'i32'),(8,8,'u64'),(16,8,'u64')])
		else:
			cmd = target.EvaluateExpression("g_WatchPointCommandInfo.cmd").signed
			flag = target.EvaluateExpression("g_WatchPointCommandInfo.flag").signed
			size = target.EvaluateExpression("g_WatchPointCommandInfo.size").signed
			addr = target.EvaluateExpression("g_WatchPointCommandInfo.addr").signed
			tid = target.EvaluateExpression("g_WatchPointCommandInfo.tid").signed

		cmd_info = ""
		if addr!=0 and size>0:
			mem_error = lldb.SBError()
			mem_val = process.ReadMemory(addr,size,mem_error)
			if mem_error.Success():
				#val = struct.unpack('q',bytearray(mem_val))
				val = int.from_bytes(bytearray(mem_val), byteorder='little')
				cmd_info = "cmd:{} flag:{} size:{} addr:{:016x} value:{:016x} tid:{}".format(cmd,flag,size,addr,val,tid)
			else:
				cmd_info = "cmd:{} flag:{} size:{} addr:{:016x} value:(failed) tid:{}".format(cmd,flag,size,addr,tid)
		else:
			cmd_info = "cmd:{} flag:{} size:{} addr:{:016x} value:(unread) tid:{}".format(cmd,flag,size,addr,tid)

		write_to_file(cmd_info)
		write_to_file('\n')
		stream.Print(cmd_info)
		stream.Print('\n')

		print("stop_reason:{} thread id:{} [trace:{} bp:{} wp:{} sig:{} exc:{} exec:{}]".format(thread.stop_reason, thread.id,
			lldb.eStopReasonTrace,
			lldb.eStopReasonBreakpoint,
			lldb.eStopReasonWatchpoint,
			lldb.eStopReasonSignal,
			lldb.eStopReasonException,
			lldb.eStopReasonExec))

		if thread.stop_reason == lldb.eStopReasonException and thread.id == tid: #macos
			sig = thread.GetStopReasonDataAtIndex(0)
			print("sig:{}".format(sig))
			if sig == 6:
				if cmd == 1:
					while target.num_watchpoints>=4:
						wp = target.watchpoint[0]
						wp.SetEnabled(False)
						target.DeleteWatchpoint(wp.GetID())
					wp_error = lldb.SBError()
					target.WatchAddress(addr,size,False,True,wp_error)
					if hook_module:
						write_variable_index_values(target, "g_WatchPointCommandInfoVK", 0, [(0,2,'i16',0)])
					else:
						target.EvaluateExpression("g_WatchPointCommandInfo.cmd=0")
				elif cmd == 2:
					for i in range(target.num_watchpoints):
						wp = target.watchpoint[i]
						if wp.GetWatchAddress() == addr:
							wp.SetEnabled(False)
							target.DeleteWatchpoint(wp.GetID())
							if hook_module:
								write_variable_index_values(target, "g_WatchPointCommandInfoVK", 0, [(0,2,'i16',0)])
							else:
								target.EvaluateExpression("g_WatchPointCommandInfo.cmd=0")
							break
		elif thread.stop_reason == lldb.eStopReasonSignal and thread.id == tid: #android
			sig = thread.GetStopReasonDataAtIndex(0)
			print("sig:{}".format(sig))
			if sig == 5:
				if cmd == 1:
					while target.num_watchpoints>=4:
						wp = target.watchpoint[0]
						wp.SetEnabled(False)
						target.DeleteWatchpoint(wp.GetID())
					wp_error = lldb.SBError()
					target.WatchAddress(addr,size,False,True,wp_error)
					if hook_module:
						write_variable_index_values(target, "g_WatchPointCommandInfoVK", 0, [(0,2,'i16',0)])
					else:
						target.EvaluateExpression("g_WatchPointCommandInfo.cmd=0")
				elif cmd == 2:
					for i in range(target.num_watchpoints):
						wp = target.watchpoint[i]
						if wp.GetWatchAddress() == addr:
							wp.SetEnabled(False)
							target.DeleteWatchpoint(wp.GetID())
							if hook_module:
								write_variable_index_values(target, "g_WatchPointCommandInfoVK", 0, [(0,2,'i16',0)])
							else:
								target.EvaluateExpression("g_WatchPointCommandInfo.cmd=0")
							break

		wp_info = "num_watchpoints:{}".format(target.num_watchpoints)
		write_to_file(wp_info)
		write_to_file('\n')
		stream.Print(wp_info)
		stream.Print('\n')

		ret = False
		if thread.stop_reason == lldb.eStopReasonWatchpoint:
			wp_id = thread.GetStopReasonDataAtIndex(0)
			wp = target.FindWatchpointByID(wp_id)
			if wp is not None:
				wp_addr = wp.GetWatchAddress()
				wp_size = wp.GetWatchSize()
				wp_spec = wp.GetWatchSpec()
				wp_error = lldb.SBError()
				mem_val = process.ReadMemory(wp_addr,wp_size,wp_error)
				if wp_error.Success():
					#wp_val = struct.unpack('q',bytearray(mem_val))
					wp_val = int.from_bytes(bytearray(mem_val), byteorder='little')
					wp_info = "watch point addr:{:016x} size:{} spec:{} value:{:016x}".format(wp_addr,wp_size,wp_spec,wp_val)
					write_to_file(wp_info)
					write_to_file('\n')
					stream.Print(wp_info)
					stream.Print('\n')
		elif thread.stop_reason == lldb.eStopReasonException: #macos
			sig = thread.GetStopReasonDataAtIndex(0)
			sig_info = "sig:{}".format(sig)
			write_to_file(sig_info)
			write_to_file('\n')
			stream.Print(sig_info)
			stream.Print('\n')
			pass
		elif thread.stop_reason == lldb.eStopReasonSignal: #android
			sig = thread.GetStopReasonDataAtIndex(0)
			sig_info = "sig:{}".format(sig)
			write_to_file(sig_info)
			write_to_file('\n')
			stream.Print(sig_info)
			stream.Print('\n')
			if sig == 5:
				pass
			else:
				ret = True
		elif thread.stop_reason == lldb.eStopReasonThreadExiting:
			ret = True
		else:
			pass

		flush_file()
		return ret