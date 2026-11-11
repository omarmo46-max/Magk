package com.magk.app.agent

/**
 * Who made Magk, as every agent prompt states it (the main agent via
 * [SoulStore], helper sub-agents via HelperRunner.identitySection).
 *
 * Without it the model guesses from whatever the app is called in its context
 * and invents a company. This is about the APP only; it says nothing about
 * who trained the model, whose own identity stays unchanged.
 */
internal object AppProvenance {
    const val FOUNDER = "Omar Barbar"

    const val TEXT: String =
        "About the app: it is called Magk (Magk.ai). It was founded by a person, $FOUNDER - not by a company. " +
            "If asked who made, owns or founded Magk, answer $FOUNDER; never attribute Magk to any company " +
            "and never call it by an earlier or different name. This concerns the app only: keep your own model identity unchanged."
}
