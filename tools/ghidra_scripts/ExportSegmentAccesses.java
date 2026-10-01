// Exports every instruction that uses an FS segment override, so the runtime knows which Xbox KPCR/KTHREAD fields are read.
// Usage from headless:
//   -postScript ExportSegmentAccesses.java <output-csv>

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class ExportSegmentAccesses extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: <output-csv>");
        }

        try (PrintWriter writer = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8.name())) {
            writer.println("address,function_entry,function_name,instruction");
            InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
            while (instructions.hasNext() && !monitor.isCancelled()) {
                Instruction instruction = instructions.next();
                String text = instruction.toString();
                if (!text.contains("FS:")) {
                    continue;
                }
                Function function = currentProgram.getFunctionManager().getFunctionContaining(instruction.getAddress());
                writer.printf("\"%s\",\"%s\",\"%s\",\"%s\"%n",
                    instruction.getAddress(),
                    function == null ? "" : function.getEntryPoint(),
                    function == null ? "" : function.getName(),
                    text.replace("\"", "\"\""));
            }
        }
    }
}
