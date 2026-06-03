/* Nordstjernen Java API — runnable example / smoke test.
 *
 * Compile against the library jar (or classes) and run with the native libs on
 * the path, e.g.:
 *   javac -cp nordstjernen-java.jar examples/Example.java -d out
 *   java -Dnordstjernen.native.dir=path/to/native -cp nordstjernen-java.jar:out \
 *        Example https://example.com
 */

import org.nordstjernen.Nordstjernen;
import org.nordstjernen.Page;
import org.nordstjernen.Size;

import java.nio.file.Path;

public final class Example {

    public static void main(String[] args) {
        String url = args.length > 0 ? args[0] : "about:start";
        try (Page page = Nordstjernen.open(url, 360, 600)) {
            System.out.println("title: " + page.title());
            System.out.println("url:   " + page.url());
            Size size = page.pageSize();
            System.out.println("size:  " + size.width() + "x" + size.height() + " css px");

            String text = page.text();
            if (text != null) {
                String oneLine = text.replace('\n', ' ').strip();
                System.out.println("text:  "
                        + oneLine.substring(0, Math.min(160, oneLine.length())));
            }

            var links = page.links();
            System.out.println("links: " + links.size() + " found");
            links.stream().limit(5).forEach(l -> System.out.println("       " + l));

            var img = page.renderFullPage(2.0);
            System.out.println("full:  " + img.getWidth() + "x" + img.getHeight() + " px @2x");

            Path png = Path.of("example.png");
            page.renderToFile(png);
            System.out.println("wrote: " + png.toAbsolutePath());
        } finally {
            Nordstjernen.shutdown();
        }
    }
}
