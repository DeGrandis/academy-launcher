// Exports instructions for selected functions so startup control flow can be traced.
// Usage from headless:
//   -postScript ExportStartupTrace.java <output-directory> [function-entry-address...]

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public class ExportStartupTrace extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("Expected output directory argument");
        }

        File outputDir = new File(args[0]);
        if (!outputDir.exists() && !outputDir.mkdirs()) {
            throw new IllegalStateException("Could not create output directory: " + outputDir);
        }

        List<Address> entries = new ArrayList<>();
        if (args.length == 1) {
            entries.add(toAddr("00160eba"));
            entries.add(toAddr("00224100"));
        } else {
            for (int index = 1; index < args.length; ++index) {
                entries.add(toAddr(args[index]));
            }
        }

        File outputFile = new File(outputDir, "startup_trace.csv");
        try (PrintWriter writer = new PrintWriter(outputFile, StandardCharsets.UTF_8.name())) {
            writer.println("function_entry,function_name,address,bytes,mnemonic,operands,flow_type,operand_refs");
            for (Address entry : entries) {
                exportFunction(writer, entry);
            }
        }

        println("Exported startup trace to " + outputFile.getAbsolutePath());
    }

    private void exportFunction(PrintWriter writer, Address entry) throws Exception {
        Function function = currentProgram.getFunctionManager().getFunctionContaining(entry);
        if (function == null && currentProgram.getListing().getInstructionAt(entry) == null) {
            disassemble(entry);
            function = createFunction(entry, null);
        }
        if (function == null) {
            println("No function contains " + entry + "; exporting bounded instruction range");
            exportInstructionRange(writer, entry, 96);
            return;
        }

        InstructionIterator instructions = currentProgram.getListing().getInstructions(function.getBody(), true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            writer.printf("%s,%s,%s,%s,%s,%s,%s,%s%n",
                csv(function.getEntryPoint().toString()),
                csv(function.getName()),
                csv(instruction.getAddress().toString()),
                csv(formatBytes(instruction.getBytes())),
                csv(instruction.getMnemonicString()),
                csv(instruction.toString()),
                csv(instruction.getFlowType().toString()),
                csv(formatReferences(instruction.getReferencesFrom())));
        }
    }

    private void exportInstructionRange(PrintWriter writer, Address entry, int maxInstructions) throws Exception {
        InstructionIterator instructions = currentProgram.getListing().getInstructions(entry, true);
        int count = 0;
        while (instructions.hasNext() && count < maxInstructions && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            writer.printf("%s,%s,%s,%s,%s,%s,%s,%s%n",
                csv(entry.toString()),
                csv("range_" + entry),
                csv(instruction.getAddress().toString()),
                csv(formatBytes(instruction.getBytes())),
                csv(instruction.getMnemonicString()),
                csv(instruction.toString()),
                csv(instruction.getFlowType().toString()),
                csv(formatReferences(instruction.getReferencesFrom())));
            ++count;

            if (instruction.getFlowType().isTerminal()) {
                break;
            }
        }
    }

    private String formatBytes(byte[] bytes) {
        StringBuilder builder = new StringBuilder();
        for (byte value : bytes) {
            if (builder.length() > 0) {
                builder.append(" ");
            }
            builder.append(String.format("%02x", value & 0xff));
        }
        return builder.toString();
    }

    private String formatReferences(Reference[] references) {
        StringBuilder builder = new StringBuilder();
        for (Reference reference : references) {
            if (builder.length() > 0) {
                builder.append(";");
            }
            builder.append(reference.getReferenceType()).append(":").append(reference.getToAddress());
        }
        return builder.toString();
    }

    private String csv(String value) {
        String escaped = value == null ? "" : value.replace("\"", "\"\"");
        return "\"" + escaped + "\"";
    }
}
