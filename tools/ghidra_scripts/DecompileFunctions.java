// Decompiles functions and lists references, for reading game code without the Ghidra UI.
// Usage from headless:
//   -postScript DecompileFunctions.java <output-file> <item> [<item> ...]
// Each item is one of:
//   <address>        decompile the function containing the address
//   refs:<address>   list every reference to the address (with the referencing function)
//   callers:<address> decompile every function that references the address

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileFunctions extends GhidraScript {
    private DecompInterface decompiler;
    private PrintWriter writer;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("usage: <output-file> <item>...");
        }
        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8.name())) {
            writer = out;
            for (int index = 1; index < args.length; ++index) {
                String item = args[index];
                if (item.startsWith("refs:")) {
                    listReferences(toAddr(item.substring(5)));
                } else if (item.startsWith("callers:")) {
                    Set<Function> callers = new LinkedHashSet<>();
                    for (Reference reference : getReferencesTo(toAddr(item.substring(8)))) {
                        Function caller = getFunctionContaining(reference.getFromAddress());
                        if (caller != null) {
                            callers.add(caller);
                        }
                    }
                    for (Function caller : callers) {
                        decompile(caller);
                    }
                } else {
                    Address address = toAddr(item);
                    Function function = getFunctionContaining(address);
                    if (function == null) {
                        writer.println("// no function contains " + address);
                    } else {
                        decompile(function);
                    }
                }
            }
        } finally {
            decompiler.dispose();
        }
    }

    private void listReferences(Address target) {
        writer.println("// references to " + target);
        for (Reference reference : getReferencesTo(target)) {
            Function from = getFunctionContaining(reference.getFromAddress());
            writer.println("//   " + reference.getFromAddress() + " " + reference.getReferenceType()
                + (from == null ? "" : " in " + from.getName() + " @ " + from.getEntryPoint()));
        }
        writer.println();
    }

    private void decompile(Function function) {
        writer.println("// ===== " + function.getName() + " @ " + function.getEntryPoint());
        DecompileResults results = decompiler.decompileFunction(function, 60, monitor);
        if (results.decompileCompleted()) {
            writer.println(results.getDecompiledFunction().getC());
        } else {
            writer.println("// decompile failed: " + results.getErrorMessage());
        }
    }
}
