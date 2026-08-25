package com.mcoverlay.attach;

import com.sun.tools.attach.AgentInitializationException;
import com.sun.tools.attach.AgentLoadException;
import com.sun.tools.attach.AttachNotSupportedException;
import com.sun.tools.attach.VirtualMachine;

import java.io.File;
import java.io.IOException;
import java.util.Locale;

/**
 * Loads a native JVMTI agent into an already-running JVM using the supported
 * JDK Attach API. This helper intentionally does not perform remote-thread DLL
 * injection, manual mapping, or any attempt to hide the loaded native module.
 */
public final class AttachHelper {
    private static final int EXIT_SUCCESS = 0;
    private static final int EXIT_USAGE = 2;
    private static final int EXIT_INVALID_PID = 3;
    private static final int EXIT_INVALID_AGENT_PATH = 4;
    private static final int EXIT_ATTACH_NOT_SUPPORTED = 10;
    private static final int EXIT_ATTACH_IO_FAILURE = 11;
    private static final int EXIT_AGENT_LOAD_FAILURE = 12;
    private static final int EXIT_AGENT_INITIALIZATION_FAILURE = 13;
    private static final int EXIT_SECURITY_FAILURE = 14;
    private static final int EXIT_DETACH_FAILURE = 15;
    private static final int EXIT_UNEXPECTED_FAILURE = 20;

    // Windows process identifiers are unsigned 32-bit values.
    private static final long MAX_WINDOWS_PID = 0xFFFFFFFFL;

    private AttachHelper() {
    }

    public static void main(String[] args) {
        System.exit(run(args));
    }

    static int run(String[] args) {
        if (args.length == 1 && ("--help".equals(args[0]) || "-h".equals(args[0]))) {
            printUsage();
            return EXIT_SUCCESS;
        }

        if (args.length < 2 || args.length > 3) {
            error("expected a PID, an absolute agent DLL path, and optionally one options string");
            printUsage();
            return EXIT_USAGE;
        }

        String pid = validatePid(args[0]);
        if (pid == null) {
            return EXIT_INVALID_PID;
        }

        String agentPath = validateAgentPath(args[1]);
        if (agentPath == null) {
            return EXIT_INVALID_AGENT_PATH;
        }

        // Passing null distinguishes "no options" from an explicitly supplied
        // empty options string, which is significant to some Agent_OnAttach
        // implementations.
        String options = args.length == 3 ? args[2] : null;

        VirtualMachine virtualMachine;
        try {
            virtualMachine = VirtualMachine.attach(pid);
        } catch (AttachNotSupportedException exception) {
            error("PID " + pid + " is not an attachable JVM: " + messageOf(exception));
            return EXIT_ATTACH_NOT_SUPPORTED;
        } catch (IOException exception) {
            error("could not attach to PID " + pid + ": " + messageOf(exception));
            return EXIT_ATTACH_IO_FAILURE;
        } catch (SecurityException exception) {
            error("permission denied while attaching to PID " + pid + ": " + messageOf(exception));
            return EXIT_SECURITY_FAILURE;
        } catch (RuntimeException exception) {
            error("unexpected attach failure (" + exception.getClass().getSimpleName() + "): "
                    + messageOf(exception));
            return EXIT_UNEXPECTED_FAILURE;
        }

        int result = EXIT_SUCCESS;
        boolean agentLoaded = false;
        try {
            virtualMachine.loadAgentPath(agentPath, options);
            agentLoaded = true;
        } catch (AgentLoadException exception) {
            error("the JVM could not load agent '" + agentPath + "': " + messageOf(exception));
            result = EXIT_AGENT_LOAD_FAILURE;
        } catch (AgentInitializationException exception) {
            error("Agent_OnAttach failed for '" + agentPath + "' with agent result "
                    + exception.returnValue() + ": " + messageOf(exception));
            result = EXIT_AGENT_INITIALIZATION_FAILURE;
        } catch (IOException exception) {
            error("I/O failure while loading agent into PID " + pid + ": " + messageOf(exception));
            result = EXIT_AGENT_LOAD_FAILURE;
        } catch (SecurityException exception) {
            error("permission denied while loading the agent into PID " + pid + ": "
                    + messageOf(exception));
            result = EXIT_SECURITY_FAILURE;
        } catch (RuntimeException exception) {
            error("unexpected agent-load failure (" + exception.getClass().getSimpleName() + "): "
                    + messageOf(exception));
            result = EXIT_UNEXPECTED_FAILURE;
        } finally {
            try {
                virtualMachine.detach();
            } catch (IOException exception) {
                error("warning: failed to detach cleanly from PID " + pid + ": "
                        + messageOf(exception));
                if (result == EXIT_SUCCESS) {
                    result = EXIT_DETACH_FAILURE;
                }
            }
        }

        if (agentLoaded && result == EXIT_SUCCESS) {
            System.out.println("JVMTI agent loaded successfully into PID " + pid + ".");
        }

        return result;
    }

    private static String validatePid(String candidate) {
        if (candidate == null || candidate.length() == 0) {
            error("PID must be a positive decimal integer");
            return null;
        }

        for (int index = 0; index < candidate.length(); ++index) {
            char character = candidate.charAt(index);
            if (character < '0' || character > '9') {
                error("invalid PID '" + candidate + "': expected decimal digits only");
                return null;
            }
        }

        try {
            long numericPid = Long.parseLong(candidate);
            if (numericPid == 0L || numericPid > MAX_WINDOWS_PID) {
                error("invalid PID '" + candidate + "': expected a value from 1 to "
                        + MAX_WINDOWS_PID);
                return null;
            }
        } catch (NumberFormatException exception) {
            error("invalid PID '" + candidate + "': value is outside the supported range");
            return null;
        }

        // Normalize leading zeroes so the Attach provider receives the same
        // decimal identifier that Windows reports for the process.
        return Long.toString(Long.parseLong(candidate));
    }

    private static String validateAgentPath(String candidate) {
        if (candidate == null || candidate.length() == 0) {
            error("agent path must not be empty");
            return null;
        }

        File suppliedPath = new File(candidate);
        if (!suppliedPath.isAbsolute()) {
            error("agent path must be absolute: '" + candidate + "'");
            return null;
        }

        File canonicalPath;
        try {
            canonicalPath = suppliedPath.getCanonicalFile();
        } catch (IOException exception) {
            error("could not resolve agent path '" + candidate + "': " + messageOf(exception));
            return null;
        }

        if (!canonicalPath.getName().toLowerCase(Locale.ROOT).endsWith(".dll")) {
            error("agent path must identify a Windows .dll file: '" + canonicalPath + "'");
            return null;
        }
        if (!canonicalPath.isFile()) {
            error("agent DLL does not exist or is not a regular file: '" + canonicalPath + "'");
            return null;
        }
        if (!canonicalPath.canRead()) {
            error("agent DLL is not readable: '" + canonicalPath + "'");
            return null;
        }

        return canonicalPath.getAbsolutePath();
    }

    private static String messageOf(Throwable throwable) {
        String message = throwable.getMessage();
        return message == null || message.length() == 0 ? "no additional details" : message;
    }

    private static void error(String message) {
        System.err.println("mc-overlay-attach: " + message);
    }

    private static void printUsage() {
        System.err.println("Usage: java --add-modules jdk.attach -jar McOverlayAttachHelper.jar "
                + "<pid> <absolute-agent-dll> [agent-options]");
        System.err.println("Quote agent-options as one argument when it contains spaces.");
    }
}
