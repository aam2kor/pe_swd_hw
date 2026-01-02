import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class event_writer {

    private static final String FIFO_PATH = "motion_fifo";

    private static int eventCounter = 1;

    public static void main(String[] args) throws Exception {

        OutputStream out = new FileOutputStream(FIFO_PATH);
        ScheduledExecutorService scheduler = Executors.newScheduledThreadPool(3);

        // events every 5 seconds
        scheduler.scheduleAtFixedRate(() -> writeEvent(out, "event" + eventCounter++),
                0, 5, TimeUnit.SECONDS);

    }

    private static synchronized void writeEvent(OutputStream out, String event) {
        try {
            out.write((event + "\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
            System.out.println("Java: " + event + " generated");
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}

