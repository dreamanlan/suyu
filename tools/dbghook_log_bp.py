"""
summary: programmatically drive a debugging session

description:
  Start a debugging session, step through the first five
  instructions. Each instruction is disassembled after
  execution.
"""

import ctypes
import ida_idd
import ida_dbg
import ida_ida
import ida_lines
import ida_ieee

class MyDbgHook(ida_dbg.DBG_Hooks):
    """ Own debug hook class that implementd the callback functions """

    def __init__(self):
        ida_dbg.DBG_Hooks.__init__(self) # important
        self.steps = 0

    def log(self, msg):
        print(">>> %s" % msg)

    def dbg_process_start(self, pid, tid, ea, name, base, size):
        self.log("Process started, pid=%d tid=%d name=%s" % (pid, tid, name))

    def dbg_process_exit(self, pid, tid, ea, code):
        self.log("Process exited pid=%d tid=%d ea=0x%x code=%d" % (pid, tid, ea, code))

    def dbg_library_unload(self, pid, tid, ea, info):
        self.log("Library unloaded: pid=%d tid=%d ea=0x%x info=%s" % (pid, tid, ea, info))

    def dbg_process_attach(self, pid, tid, ea, name, base, size):
        self.log("Process attach pid=%d tid=%d ea=0x%x name=%s base=%x size=%x" % (pid, tid, ea, name, base, size))

    def dbg_process_detach(self, pid, tid, ea):
        self.log("Process detached, pid=%d tid=%d ea=0x%x" % (pid, tid, ea))

    def dbg_library_load(self, pid, tid, ea, name, base, size):
        self.log("Library loaded: pid=%d tid=%d name=%s base=%x" % (pid, tid, name, base))

    def dbg_bpt(self, tid, ea):
        self.log("Break point at 0x%x tid=%d" % (ea, tid))

        self.log("=== registers ===")
        dbg = ida_idd.get_dbg()
        regvals = ida_dbg.get_reg_vals(tid)
        for ridx, regval in enumerate(regvals):
            rinfo = dbg.regs(ridx)
            rval = regval.pyval(rinfo.dtype)
            if isinstance(rval, int):
                rval = "0x%x" % rval
            else:
                f1 = 0.0
                f2 = 0.0
                f3 = 0.0
                f4 = 0.0
                d1 = 0.0
                d2 = 0.0
                num = regval.get_data_size()
                if num==16:
                    bytes = regval.bytes()
                    data = bytearray(bytes)
                    fvals = struct.unpack("<4f", data)
                    dvals = struct.unpack("<2d", data)
                    f1 = fvals[0]
                    f2 = fvals[1]
                    f3 = fvals[2]
                    f4 = fvals[3]
                    d1 = dvals[0]
                    d2 = dvals[1]
                rval = ("ival:%x size:%u fval:%f %f %f %f %f %f" % (regval.ival, regval.get_data_size(), f1, f2, f3, f4, d1, d2))
            self.log("    %s: %s" % (rinfo.name, rval))
        self.log("=== end of registers  ===")

        self.log("=== start of call stack impression ===")
        trace = ida_idd.call_stack_t()
        if ida_dbg.collect_stack_trace(tid, trace):
            for frame in trace:
                mi = ida_idd.modinfo_t()
                if ida_dbg.get_module_info(frame.callea, mi):
                    module = os.path.basename(mi.name)
                    name = ida_name.get_nice_colored_name(
                        frame.callea,
                        ida_name.GNCN_NOCOLOR|ida_name.GNCN_NOLABEL|ida_name.GNCN_NOSEG|ida_name.GNCN_PREFDBG)
                    self.log("    " + hex(frame.callea) + " from: " + module + " with debug name: " + name)
                else:
                    self.log("    " + hex(frame.callea))
        self.log("===  end of call stack impression  ===")

        # return values:
        #   -1 - to display a breakpoint warning dialog
        #        if the process is suspended.
        #    0 - to never display a breakpoint warning dialog.
        #    1 - to always display a breakpoint warning dialog.
        ida_dbg.continue_process()
        return 0

    def dbg_suspend_process(self):
        self.log("Process suspended")

    def dbg_exception(self, pid, tid, ea, exc_code, exc_can_cont, exc_ea, exc_info):
        self.log("Exception: pid=%d tid=%d ea=0x%x exc_code=0x%x can_continue=%d exc_ea=0x%x exc_info=%s" % (
            pid, tid, ea, exc_code & ida_idaapi.BADADDR, exc_can_cont, exc_ea, exc_info))
        # return values:
        #   -1 - to display an exception warning dialog
        #        if the process is suspended.
        #   0  - to never display an exception warning dialog.
        #   1  - to always display an exception warning dialog.
        return 0

    def dbg_trace(self, tid, ea):
        self.log("Trace tid=%d ea=0x%x" % (tid, ea))
        # return values:
        #   1  - do not log this trace event;
        #   0  - log it
        return 0

    def dbg_step_into(self):
        self.log("Step into")

    def dbg_run_to(self, pid, tid=0, ea=0):
        self.log("Runto: tid=%d, ea=%x" % (tid, ea))

    def dbg_step_over(self):
        pc = ida_dbg.get_reg_val("PC")
        disasm = ida_lines.tag_remove(
            ida_lines.generate_disasm_line(
                pc))
        self.log("Step over: PC=0x%x, disassembly=%s" % (pc, disasm))

# Remove an existing debug hook
try:
    if debughook:
        print("Removing previous hook ...")
        debughook.unhook()
except:
    pass

# Install the debug hook
debughook = MyDbgHook()
debughook.hook()

