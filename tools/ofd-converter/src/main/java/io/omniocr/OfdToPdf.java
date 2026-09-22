package io.omniocr;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import org.ofdrw.converter.export.PDFExporterPDFBox;

/** Isolated OFD renderer: the OCR pipeline and scheduling remain in C++. */
public final class OfdToPdf {
    public static void main(String[] args) {
        if (args.length != 2) {
            System.err.println("usage: omniocr-ofd-to-pdf INPUT.ofd OUTPUT.pdf");
            System.exit(2);
        }
        Path input = Paths.get(args[0]);
        Path output = Paths.get(args[1]);
        if (!Files.isRegularFile(input) || Files.exists(output)) {
            System.err.println("input must exist and output must not exist");
            System.exit(2);
        }
        try {
            try (PDFExporterPDFBox exporter = new PDFExporterPDFBox(input, output)) {
                exporter.export();
            }
            if (!Files.isRegularFile(output) || Files.size(output) == 0) {
                throw new IllegalStateException("converter produced no PDF");
            }
        } catch (Exception failure) {
            try { Files.deleteIfExists(output); } catch (Exception ignored) { }
            System.err.println("OFD conversion failed: " + failure);
            System.exit(1);
        }
    }
}
