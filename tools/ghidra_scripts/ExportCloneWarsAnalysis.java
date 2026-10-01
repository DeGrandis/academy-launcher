// Exports a small, copyright-safe analysis index for the imported XBE.
// Usage from headless:
//   -postScript ExportCloneWarsAnalysis.java <output-directory>

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.Set;

public class ExportCloneWarsAnalysis extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outputDir = args.length > 0 ? new File(args[0]) : askDirectory("Analysis export directory", "Export");
        if (!outputDir.exists() && !outputDir.mkdirs()) {
            throw new IllegalStateException("Could not create output directory: " + outputDir);
        }

        exportFunctions(new File(outputDir, "functions.csv"));
        exportStrings(new File(outputDir, "strings.csv"));
        exportSymbols(new File(outputDir, "symbols.csv"));
        exportCalls(new File(outputDir, "calls.csv"));
        exportMemoryMap(new File(outputDir, "memory_blocks.csv"));
        exportEntrypoints(new File(outputDir, "entrypoints.csv"));

        println("Exported Clone Wars analysis index to " + outputDir.getAbsolutePath());
    }

    private void exportFunctions(File outputFile) throws Exception {
        try (PrintWriter writer = writer(outputFile)) {
            writer.println("entry,name,namespace,body_min,body_max,body_bytes,parameter_count,calling_convention");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            for (Function function : functions) {
                AddressSetView body = function.getBody();
                writer.printf("%s,%s,%s,%s,%s,%d,%d,%s%n",
                    csv(function.getEntryPoint().toString()),
                    csv(function.getName()),
                    csv(function.getParentNamespace().getName(true)),
                    csv(body.getMinAddress().toString()),
                    csv(body.getMaxAddress().toString()),
                    body.getNumAddresses(),
                    function.getParameterCount(),
                    csv(function.getCallingConventionName()));
            }
        }
    }

    private void exportStrings(File outputFile) throws Exception {
        try (PrintWriter writer = writer(outputFile)) {
            writer.println("address,length,value");
            DataIterator dataIterator = currentProgram.getListing().getDefinedData(true);
            while (dataIterator.hasNext()) {
                Data data = dataIterator.next();
                Object value = data.getValue();
                if (!(value instanceof String)) {
                    continue;
                }
                writer.printf("%s,%d,%s%n",
                    csv(data.getAddress().toString()),
                    data.getLength(),
                    csv(value == null ? "" : value.toString()));
            }
        }
    }

    private void exportSymbols(File outputFile) throws Exception {
        try (PrintWriter writer = writer(outputFile)) {
            writer.println("address,name,namespace,type,source,reference_count");
            SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
            for (Symbol symbol : symbols) {
                Reference[] references = symbol.getReferences();
                writer.printf("%s,%s,%s,%s,%s,%d%n",
                    csv(symbol.getAddress() == null ? "" : symbol.getAddress().toString()),
                    csv(symbol.getName()),
                    csv(symbol.getParentNamespace().getName(true)),
                    csv(symbol.getSymbolType().toString()),
                    csv(symbol.getSource().toString()),
                    references == null ? 0 : references.length);
            }
        }
    }

    private void exportMemoryMap(File outputFile) throws Exception {
        try (PrintWriter writer = writer(outputFile)) {
            writer.println("name,start,end,size,read,write,execute,initialized");
            for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
                writer.printf("%s,%s,%s,%d,%s,%s,%s,%s%n",
                    csv(block.getName()),
                    csv(block.getStart().toString()),
                    csv(block.getEnd().toString()),
                    block.getSize(),
                    block.isRead(),
                    block.isWrite(),
                    block.isExecute(),
                    block.isInitialized());
            }
        }
    }

    private void exportCalls(File outputFile) throws Exception {
        try (PrintWriter writer = writer(outputFile)) {
            writer.println("caller_entry,caller_name,callee_entry,callee_name,callee_namespace");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            for (Function function : functions) {
                Set<Function> calledFunctions = function.getCalledFunctions(monitor);
                for (Function calledFunction : calledFunctions) {
                    writer.printf("%s,%s,%s,%s,%s%n",
                        csv(function.getEntryPoint().toString()),
                        csv(function.getName()),
                        csv(calledFunction.getEntryPoint().toString()),
                        csv(calledFunction.getName()),
                        csv(calledFunction.getParentNamespace().getName(true)));
                }
            }
        }
    }

    private void exportEntrypoints(File outputFile) throws Exception {
        try (PrintWriter writer = writer(outputFile)) {
            writer.println("address");
            AddressIterator addresses = currentProgram.getSymbolTable().getExternalEntryPointIterator();
            while (addresses.hasNext()) {
                Address address = addresses.next();
                writer.println(csv(address.toString()));
            }
        }
    }

    private PrintWriter writer(File file) throws Exception {
        return new PrintWriter(file, StandardCharsets.UTF_8.name());
    }

    private String csv(String value) {
        String escaped = value == null ? "" : value.replace("\"", "\"\"");
        return "\"" + escaped + "\"";
    }
}
