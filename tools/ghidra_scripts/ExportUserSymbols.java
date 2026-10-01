// Exports names typed in the Ghidra UI (SourceType.USER_DEFINED) to symbols/ghidra_user.csv, so they live in git.
// Usage from headless:
//   -postScript ExportUserSymbols.java <repo-root>
// gen_header.py and ImportSymbols.java read the file; move names into symbols/manual.csv to add types or comments.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolType;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.TreeMap;

public class ExportUserSymbols extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File root = new File(args.length > 0 ? args[0] : ".");
        TreeMap<Long, String> rows = new TreeMap<>();
        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        for (Symbol symbol : symbols) {
            if (symbol.getSource() != SourceType.USER_DEFINED || symbol.isExternal()) {
                continue;
            }
            boolean isFunction = symbol.getSymbolType() == SymbolType.FUNCTION;
            if (!isFunction && symbol.getSymbolType() != SymbolType.LABEL) {
                continue;
            }
            String comment;
            if (isFunction) {
                Function function = getFunctionAt(symbol.getAddress());
                comment = function != null && function.getComment() != null ? function.getComment() : "";
            } else {
                String eol = currentProgram.getListing().getComment(CodeUnit.EOL_COMMENT, symbol.getAddress());
                comment = eol != null ? eol : "";
            }
            String name = symbol.getName(true);
            rows.put(symbol.getAddress().getOffset(), String.format("%s,%08X,%s,,%s", isFunction ? "function" : "global",
                symbol.getAddress().getOffset(), quote(name), quote(comment.replace('\n', ' '))));
        }
        File out = new File(root, "symbols/ghidra_user.csv");
        try (PrintWriter writer = new PrintWriter(out, StandardCharsets.UTF_8.name())) {
            writer.println("kind,address,name,type,comment");
            for (String row : rows.values()) {
                writer.println(row);
            }
        }
        println("ExportUserSymbols: wrote " + rows.size() + " names to " + out);
    }

    private static String quote(String text) {
        return text.contains(",") || text.contains("\"") ? "\"" + text.replace("\"", "\"\"") + "\"" : text;
    }
}
