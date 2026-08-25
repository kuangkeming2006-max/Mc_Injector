package com.mcoverlay.tests;

import java.awt.Toolkit;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * Private JVM used by Run-AgentAttachSmoke.ps1.
 *
 * <p>The process deliberately does not create an OpenGL context. Initializing
 * AWT only ensures that gdi32.dll is present, matching the prerequisite for
 * installing the SwapBuffers hook without producing a visible test window.
 * After reporting readiness, the process waits for QUIT on standard input.</p>
 */
public final class AttachSmokeTarget {
    private static final String READY = "MC_OVERLAY_ATTACH_SMOKE_READY";

    private AttachSmokeTarget() {
    }

    public static void main(String[] args) throws Exception {
        Toolkit.getDefaultToolkit();

        System.out.println(READY);
        System.out.flush();

        BufferedReader input = new BufferedReader(
                new InputStreamReader(System.in, StandardCharsets.UTF_8));
        while (true) {
            String command = input.readLine();
            if (command == null || "QUIT".equals(command)) {
                return;
            }
        }
    }
}
