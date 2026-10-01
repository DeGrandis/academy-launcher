// Applies the text symbol database (symbols/*.csv) to the Ghidra program.
// Usage from headless (without -readOnly so the names are saved):
//   -postScript ImportSymbols.java <repo-root>
// Names are applied with SourceType.IMPORTED, so names typed in the Ghidra UI (USER_DEFINED) are left alone
// and can be exported with ExportUserSymbols.java. "Class::method" names create class namespaces.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

public class ImportSymbols extends GhidraScript {
    private int applied;
    private int skipped;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File root = new File(args.length > 0 ? args[0] : ".");
        // Later files win: generated names first, curated names last.
        apply(new File(root, "symbols/auto/globals.csv"), "global");
        apply(new File(root, "symbols/auto/functions.csv"), "function");
        apply(new File(root, "symbols/ghidra_user.csv"), null);
        apply(new File(root, "symbols/manual.csv"), null);
        println("ImportSymbols: applied " + applied + ", skipped " + skipped);
    }

    private void apply(File file, String fixedKind) throws Exception {
        if (!file.exists()) {
            return;
        }
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            List<String> header = parse(reader.readLine());
            String line;
            while ((line = reader.readLine()) != null) {
                List<String> cells = parse(line);
                String kind = fixedKind != null ? fixedKind : get(header, cells, "kind");
                Address address = toAddr(Long.parseLong(get(header, cells, "address"), 16));
                String name = get(header, cells, "name");
                String comment = get(header, cells, "comment");
                String type = get(header, cells, "type");
                if (name.isEmpty()) {
                    continue;
                }
                if ("function".equals(kind)) {
                    nameFunction(address, name, comment, type);
                } else {
                    nameLabel(address, name, comment, type);
                }
            }
        }
    }

    private void nameFunction(Address address, String fullName, String comment, String type) throws Exception {
        Function function = getFunctionAt(address);
        if (function == null) {
            disassemble(address);
            function = createFunction(address, null);
        }
        if (function == null) {
            skipped++;
            return;
        }
        if (function.getSymbol().getSource() == SourceType.USER_DEFINED) {
            skipped++;
            return;
        }
        Namespace namespace = currentProgram.getGlobalNamespace();
        String name = fullName;
        int separator = fullName.lastIndexOf("::");
        if (separator > 0) {
            namespace = classNamespace(fullName.substring(0, separator));
            name = fullName.substring(separator + 2);
        }
        function.setParentNamespace(namespace);
        function.setName(name, SourceType.IMPORTED);
        String text = comment + (type.isEmpty() ? "" : (comment.isEmpty() ? "" : "\n") + "signature: " + type.replace(';', ','));
        if (!text.isEmpty()) {
            function.setComment(text);
        }
        applied++;
    }

    private void nameLabel(Address address, String name, String comment, String type) throws Exception {
        SymbolTable table = currentProgram.getSymbolTable();
        Symbol primary = table.getPrimarySymbol(address);
        if (primary != null && primary.getSource() == SourceType.USER_DEFINED) {
            skipped++;
            return;
        }
        Symbol symbol = table.createLabel(address, name, SourceType.IMPORTED);
        symbol.setPrimary();
        String text = comment + (type.isEmpty() ? "" : (comment.isEmpty() ? "" : " ") + "(" + type + ")");
        if (!text.isEmpty()) {
            currentProgram.getListing().setComment(address, CodeUnit.EOL_COMMENT, text);
        }
        applied++;
    }

    private Namespace classNamespace(String path) throws Exception {
        Namespace namespace = currentProgram.getGlobalNamespace();
        for (String part : path.split("::")) {
            Namespace existing = currentProgram.getSymbolTable().getNamespace(part, namespace);
            namespace = existing != null ? existing : currentProgram.getSymbolTable().createClass(namespace, part, SourceType.IMPORTED);
        }
        return namespace;
    }

    private static String get(List<String> header, List<String> cells, String column) {
        int index = header.indexOf(column);
        return index >= 0 && index < cells.size() ? cells.get(index) : "";
    }

    // Minimal CSV parsing with quoted fields.
    private static List<String> parse(String line) {
        List<String> cells = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); ++i) {
            char c = line.charAt(i);
            if (quoted) {
                if (c == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') {
                    current.append('"');
                    ++i;
                } else if (c == '"') {
                    quoted = false;
                } else {
                    current.append(c);
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == ',') {
                cells.add(current.toString());
                current.setLength(0);
            } else {
                current.append(c);
            }
        }
        cells.add(current.toString());
        return cells;
    }
}
