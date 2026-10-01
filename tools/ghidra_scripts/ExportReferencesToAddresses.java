// Exports references to selected addresses so menu/resource string users can be traced.
// Usage from headless:
//   -postScript ExportReferencesToAddresses.java <output-csv> <address> [address...]

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class ExportReferencesToAddresses extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("usage: <output-csv> <address> [address...]");
        }

        File outputFile = new File(args[0]);
        File parent = outputFile.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IllegalStateException("Could not create output directory: " + parent);
        }

        try (PrintWriter writer = new PrintWriter(outputFile, StandardCharsets.UTF_8.name())) {
            writer.println("target_address,from_address,reference_type,function_entry,function_name");
            for (int index = 1; index < args.length; ++index) {
                Address target = toAddr(args[index]);
                for (Reference reference : currentProgram.getReferenceManager().getReferencesTo(target)) {
                    Address from = reference.getFromAddress();
                    Function function = currentProgram.getFunctionManager().getFunctionContaining(from);
                    writer.printf("%s,%s,%s,%s,%s%n",
                        csv(target.toString()),
                        csv(from.toString()),
                        csv(reference.getReferenceType().toString()),
                        csv(function == null ? "" : function.getEntryPoint().toString()),
                        csv(function == null ? "" : function.getName()));
                }
            }
        }

        println("Exported selected references to " + outputFile.getAbsolutePath());
    }

    private String csv(String value) {
        String escaped = value == null ? "" : value.replace("\"", "\"\"");
        return "\"" + escaped + "\"";
    }
}