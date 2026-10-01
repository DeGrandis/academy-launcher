// Exports the observed calling convention of functions in an address range: stack bytes popped by RET
// and which registers are read before being written (register-passed arguments).
// Usage from headless:
//   -postScript ExportFunctionAbi.java <output-csv> <start-address> <end-address>

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.TreeSet;

public class ExportFunctionAbi extends GhidraScript {
    private static final String[] ARGUMENT_REGISTERS = {"EAX", "ECX", "EDX", "EBX", "ESI", "EDI"};

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            throw new IllegalArgumentException("usage: <output-csv> <start> <end>");
        }
        Address start = toAddr(args[1]);
        Address end = toAddr(args[2]);

        try (PrintWriter writer = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8.name())) {
            writer.println("entry,name,ret_purge,register_args,max_stack_arg_offset");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(start, true);
            for (Function function : functions) {
                if (function.getEntryPoint().compareTo(end) > 0) {
                    break;
                }
                Set<String> purges = new TreeSet<>();
                Set<String> written = new LinkedHashSet<>();
                Set<String> readFirst = new LinkedHashSet<>();
                int maxStackArg = 0;
                boolean linear = true;

                for (Instruction instruction : currentProgram.getListing().getInstructions(function.getBody(), true)) {
                    String mnemonic = instruction.getMnemonicString();
                    if (mnemonic.equals("RET")) {
                        Scalar purge = instruction.getNumOperands() > 0 ? instruction.getScalar(0) : null;
                        purges.add(purge == null ? "0" : Long.toString(purge.getUnsignedValue()));
                    }
                    String text = instruction.toString();
                    int espIndex = text.indexOf("ESP + 0x");
                    if (espIndex >= 0) {
                        String hex = text.substring(espIndex + 8).replaceAll("[^0-9a-fA-F].*$", "");
                        if (!hex.isEmpty()) {
                            maxStackArg = Math.max(maxStackArg, Integer.parseInt(hex, 16));
                        }
                    }
                    if (linear) {
                        for (Object input : instruction.getInputObjects()) {
                            if (input instanceof Register register) {
                                String name = register.getBaseRegister().getName();
                                for (String candidate : ARGUMENT_REGISTERS) {
                                    if (candidate.equals(name) && !written.contains(name) && !isSelfClear(instruction, name)
                                        && !mnemonic.equals("PUSH")) {
                                        readFirst.add(name);
                                    }
                                }
                            }
                        }
                        for (Object output : instruction.getResultObjects()) {
                            if (output instanceof Register register) {
                                written.add(register.getBaseRegister().getName());
                            }
                        }
                        if (instruction.getFlowType().isJump() || instruction.getFlowType().isCall() || instruction.getFlowType().isTerminal()) {
                            linear = false;
                        }
                    }
                }

                writer.printf("\"%s\",\"%s\",\"%s\",\"%s\",%d%n", function.getEntryPoint(), function.getName(),
                    String.join("|", purges), String.join("|", readFirst), maxStackArg);
            }
        }
    }

    private boolean isSelfClear(Instruction instruction, String register) {
        String mnemonic = instruction.getMnemonicString();
        return (mnemonic.equals("XOR") || mnemonic.equals("SUB")) && instruction.toString().equals(mnemonic + " " + register + "," + register);
    }
}
