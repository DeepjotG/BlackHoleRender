# Review findings (to fix later)

Found by an automated code review of the menu and the star-disruption flow (2026-10-24), stopped
early on request. Nothing here has been fixed. **Status**: *confirmed* = a second reviewer tried to
refute it against the code and could not; *unchecked* = reported by one reviewer, not yet verified.
Line numbers refer to commit f9cc2dc.
Some problems were spotted by more than one reviewer and appear in more than one section.
27 findings, 7 confirmed.


## Menu (ImGui panel)

### The star-disruption watch camera cancels or undoes every Travel, Go and bookmark flight

- **Severity:** high   **Status:** unchecked   **Where:** `src/App.cpp:971`
- **What you see:** After Spawn star, which starts the watch camera automatically, none of these do anything while the camera rides with the star: Travel > Black hole, the planet and Star buttons, Surprise me, Nearest star, list entries, a planet's Go, the Camera bookmarks, and the R/1-4 keys. The camera snaps straight back to the star. Once the debris passes pericentre, the same controls do fly to the target, but within 2-3 s the camera is dragged back to 30-50 M from the hole (about 1750 M for the red supergiant). From a galaxy 10^12 M away it rushes back across ten orders of magnitude, and riding along a planet is dropped. This lasts for the rest of the event, until Clear or until the user presses W/A/S/D or mouse-looks. The README promises 'Fly or look around at any time to take over'.
- **Why:** The only take-over test is `steering` (App.cpp:971), which counts WASD/mouse-look/roll input only. In phase 1 the camera is placed with camera_.setPose every frame (App.cpp:1005), and Camera::setPose sets transitioning_ = false (Camera.cpp:78). A flight started by goTo is handled after the watch block (App.cpp:1070-1073), so the next frame cancels it. A bookmark flyTo from onKey (App.cpp:355-359, which runs inside glfwPollEvents before updateWorld) is cancelled in the same frame. Phase 2 (App.cpp:1017-1025) only pauses while camera_.isTransitioning(). Once the user's flight lands it scales the camera position by (target/d)^(1.2 dt) every frame, with target = max(30, 5.5*pericentre) (App.cpp:1020). The 400-radius test at App.cpp:1034 then drops the ride.
- **Suggested fix:** End the watch whenever the user starts a flight. Set watchPhase_ = 0 when goToRequest_ is handled (or inside goTo()) and in the R/1-4 key cases. Route the Camera bookmark buttons (Ui.cpp:625, which call cam.flyTo directly) through an app request that does the same. Alternatively, mark the watch's own flyToTarget (App.cpp:1014) and treat any other camera transition during phases 1 and 2 as the user taking over.

### The 'Stellar (10)' mass preset hides the black hole inside giant planets, and 'Move it out' sends them out of reach

- **Severity:** medium   **Status:** unchecked   **Where:** `src/Ui.cpp:308`
- **What you see:** One click on the first mass preset makes the planets 542 M (Miller), 413 M (Mann) and 474 M (Edmunds) in radius while they orbit at 6.5/60/250 M. The spheres contain the black hole and its disk, so the camera is pushed out onto a planet surface that fills the view. Travel > Black hole and the bookmarks also end on that surface. At real time the planets orbit in 5 ms, 0.14 s and 1.2 s: the auto-ride camera shakes and Edmunds' sphere keeps sweeping over the camera and shoving it. The offered 'Move it out' sets orbits of about 107,000, 94,000 and 99,000 M. That is above the slider's 5000 M maximum, so the first touch snaps the orbit back inside the limit. In the painted-backdrop universe it is also beyond the camera's 20,000 M limit, so Go stops about 87,000 M short.
- **Why:** planetRadiusM = radiusKm*1e3/metersPerM (BlackHoleSystem.cpp:113); at 10 suns, 1 M = 14.8 km. Planets are always added as collidable bodies (App.cpp:715-739; BodyDesc::collidable defaults to true). pushOutOf runs every frame, including at the end of flights (App.cpp:1068-1069). Auto-ride attaches within 60 radii (App.cpp:1052-1058). 'Move it out' sets 1.15*rocheLimitM (Ui.cpp:396, BlackHoleSystem.cpp:117-119), about 1.07e5 M for Miller. The orbit slider range is 3.05-5000 (Ui.cpp:379), and camera_.maxRadius is 20000 in mode 0 (App.cpp:1065).
- **Suggested fix:** Do not draw or collide a planet whose radius is a large fraction of its orbit or that is inside its tidal limit; report it in the panel instead (e.g. 'torn apart at this mass'), or hide it until it is moved out. Clamp 'Move it out' to the camera's reach and widen or adapt the Orbit radius slider so the value it sets can be shown and edited.

### 'Watch' with no active star drops the camera onto the horizon and the screen goes black

- **Severity:** medium   **Status:** unchecked   **Where:** `src/App.cpp:816`
- **What you see:** Pressing Watch before any Spawn star (also possible when the simulation module is missing) puts the camera 2.3 M from the hole, looking straight in. The screen goes black, because inside the photon sphere the shadow fills the view. The panel reads 2.3 M from the hole and flight speed crawls. After Clear, or after an event ends by itself (e.g. a star swallowed whole), Watch jumps to empty space where the old star was.
- **Why:** The watchRequested branch (App.cpp:816-838) runs before the `if (!tde_.active())` return (App.cpp:839) and reads tde_.starPosition()/starRadiusM() regardless. Before any spawn these are comX = 0 (TdeSim.cpp:194) and ev.R = 0 (TdePhysics.h:329). R is clamped to 1e-6, so setPose puts the camera about 1.4e-5 M from the origin. On the next frame watchPhase_ is reset (App.cpp:972-973) and Camera::update clamps r < kMinRadius to 2.3 M (Camera.cpp:199-201). TdeSim's clear() (TdeSim.cpp:348-375) does not reset comX or ev, so after Clear the position is stale. The Watch button (Ui.cpp:451) is never disabled.
- **Suggested fix:** Drop watchRequested unless tde_.active(). Wrap the Watch button in BeginDisabled(!c.tdeActive), with a tooltip such as 'Spawn a star first'.

### 'Ride along' moves the camera by a stale offset and stops riding at once

- **Severity:** medium   **Status:** unchecked   **Where:** `src/Ui.cpp:378`
- **What you see:** From the default view, clicking Ride along on Edmunds moves the camera about 250 M sideways (Mann about 60 M, Miller about 6.5 M), so the black hole leaves the screen. The button flips straight back to 'Ride along' and nothing rides. Later clicks jump by however far the planet has moved since the last ride ended.
- **Why:** The button only writes *c.followName (Ui.cpp:378). followPrevPos_ (App.h:114, initially zero) is not updated. On the next frame the follow block calls camera_.shift(b->pos - followPrevPos_) (App.cpp:1032), which on first use moves the camera by the planet's whole position vector. The 400-radius test (App.cpp:1034; about 0.02 M at the default 1e8 suns) then clears followName_, because the camera was never near the planet.
- **Suggested fix:** Start rides in the app instead of writing followName_ from the UI, and set followPrevPos_ to the body's current position whenever a ride begins. When the camera is not already near the planet, have Ride along fly there first (as Go does through flightTarget_), or show the button only inside the ride radius.

### 'Stop riding' and the Travel 'Stop' button do nothing near a planet

- **Severity:** medium   **Status:** unchecked   **Where:** `src/App.cpp:1052`
- **What you see:** After Go to a planet the camera rides along with it. Pressing 'Stop riding' (Planets) or 'Stop' (Travel, 'Riding along with X') has no effect: the label never changes, and the user can never stay put and watch Miller sweep past at about 0.4 c.
- **Why:** Go stops at 4.5 planet radii (App.cpp:686). The buttons clear followName_ (Ui.cpp:378, 519). On the next updateWorld, the auto-ride loop attaches to any planet within 60 radii whenever followName_ is empty (App.cpp:1052-1058), so followName_ is set again before the panel is drawn.
- **Suggested fix:** Remember the planet the user detached from and skip it in the auto-ride until the camera has left its 60-radius zone. Alternatively, auto-ride only when a flight to that planet ends.

### Most Sky controls do nothing in the 3D universes but still freeze the app for a rebake

- **Severity:** medium   **Status:** unchecked   **Where:** `src/Ui.cpp:579`
- **What you see:** With 'Invented 3D universe' or 'Real 3D universe' selected, these controls have no visible effect: Sky > Seed, Random, Galaxies, Regenerate sky, Sky brightness, Galaxy brightness, Milky Way, Nebulae, Star density, and Quality > Sky resolution. Those that change the baked sky still trigger the synchronous 1-4 s painted-sky bake (the view freezes) and show the amber 'Sky rebakes when you release the control' note. Only Star brightness works.
- **Why:** In universe mode the tracer calls sampleUniverseSky (shaders/blackhole.frag:298-299). It uses only uUniCube*uUniSkyScale and the star list scaled by starBrightness (App.cpp:496). uSkyBrightness, uStarDensity and the baked cubemap are read only by sampleSky (shaders/sky.glsl:108-116). App.cpp:436-443 rebakes whenever sky_.needsRebake() is true, whatever universeMode is.
- **Suggested fix:** When s.universeMode != 0, hide or BeginDisabled() the painted-sky controls (keep Star brightness) with a note pointing to Universe > Deep-sky exposure. Skip sky_.bake until the painted backdrop is selected again.

### Real universe: changing the black hole placement keeps the old mass, while reloading Real overwrites the user's mass

- **Severity:** low   **Status:** unchecked   **Where:** `src/App.cpp:778`
- **What you see:** Switching Universe > Black hole from 'Sagittarius A*' to 'Near the Sun (fictional Gargantua)' keeps 4.3 million suns; the reverse gives Sgr A* 100 million suns. The README ties each placement to its own mass. In the other direction, a mass the user set in Real mode is replaced by the placement's value on every launch, when --mass is given, and when 'Download the real galaxies' finishes a few minutes later. That last case can happen mid star disruption: the hole's mass changes under the running event, and the planets' sizes and the panel's numbers change with it.
- **Why:** The suggested mass is applied only when enteringReal = (mode == 2 && uniModeApplied_ != 2) (App.cpp:778, 787). A placement change leaves uniModeApplied_ at 2, so the mass is not touched. uniModeApplied_ starts at -1 (App.h:99) and is reset to -1 when the download completes (App.cpp:906), so both of those count as entering Real.
- **Suggested fix:** Apply suggestedMassSolar when the user actually changes the mode or the placement. Do not apply it when the same configuration is rebuilt (startup with a loaded settings.ini or --mass, or the reload after the catalogue download).

### A mass changed after picking a scenario is silently undone by Spawn star

- **Severity:** low   **Status:** unchecked   **Where:** `src/Ui.cpp:432`
- **What you see:** Pick 'Red giant vs Gargantua', then click 'M87* (6.5 B)' (or drag Mass) to try the same star against another hole, and press Spawn star. The mass jumps back to 100 million suns. Only star or orbit edits switch the scenario to Custom, even though the Custom entry reads '(the star and orbit below, this black hole)'.
- **Why:** The Custom check compares only star mass, radius, temperature, beta, tilt and retrograde (Ui.cpp:418, 431-433). The Mass slider and presets (Ui.cpp:307-314) never change t.scenario, so Spawn re-applies the scenario's mass (App.cpp:799).
- **Suggested fix:** Remember the mass when a scenario is chosen (in the combo or at spawn). If s.bhMassSolar changes after that, set t.scenario to Custom. Keep the untouched default scenario applying its own mass on the first spawn, which is what the swallowed-whole fix relies on.

### 'Pause disk (P)' pauses the whole simulation, so a spawned star sits frozen with no hint why

- **Severity:** low   **Status:** unchecked   **Where:** `src/Ui.cpp:575`
- **What you see:** A user ticks 'Pause disk' to stop the disk's swirl. Later, Spawn star moves the camera to a star that never moves, because the planets and the event clock are stopped too. The Star disruption section shows no paused indicator, so it looks like the star disruption does nothing.
- **Why:** The checkbox writes s.paused, which zeroes both the world clock and the event clock (App.cpp:913-914). The only pause hints are in Black hole & time: the '[paused, P]' text (Ui.cpp:324-325) and a note shown only while the disk is on (Ui.cpp:339). The control that stops only the disk is Disk rotation = 0.
- **Suggested fix:** Rename the checkbox 'Pause everything (P)', or make it stop only the disk flow. Show a 'Paused (P)' note in the Star disruption section while paused.

### Swallowed-whole messages blame the hole and suggest Sagittarius A* when the real cause is a too-deep orbit

- **Severity:** low   **Status:** unchecked   **Where:** `src/Ui.cpp:465`
- **What you see:** With the default 'Sun-like star vs Sagittarius A*' and Penetration above 4.46, the preview says 'Too compact for these tides'. After spawning, the panel says 'this black hole is too big to tear this star apart (try a red giant, or Sagittarius A*)', although the hole already is Sagittarius A*. The star would be torn apart at a lower beta.
- **Why:** Both messages trigger on rp = rt/beta < 4 (Ui.cpp:440-442) and st.swallowedWhole (pericentre inside the 4 M capture radius). They do not separate rt < 4 (the hole really is too big) from rt >= 4 with too large a beta; here rt = 17.8 M.
- **Suggested fix:** When rt >= 4 but rp < 4, say the orbit dives inside the capture radius and suggest Penetration below rt/4. Keep the current wording only when rt < 4.

### Pressing Esc to close an open dropdown quits the app

- **Severity:** low   **Status:** unchecked   **Where:** `src/App.cpp:345`
- **What you see:** With the mouse free, opening a dropdown (Scenario, Universe, Preset, ...) and pressing Esc to dismiss it closes the whole simulation immediately. A running star disruption and the camera position are lost.
- **Why:** Ui::init leaves ImGui keyboard navigation off (Ui.cpp:28-32). ImGui then closes popups on Esc only through nav (imgui.cpp:14465). WantCaptureKeyboard is false while a non-modal combo popup is open (imgui.cpp:5554). onKey therefore reaches quit_ = true (App.cpp:345-349).
- **Suggested fix:** In the Esc case, if ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) is true (expose it from Ui), close the popups instead of quitting. Consider requiring a second Esc or a confirmation before quitting.

## Star disruption flow

### Spawning a star while an event is still running uses the old event's clock, so the new star skips its fall-in (a swallowed-whole star disappears before it is ever drawn)

- **Severity:** high   **Status:** unchecked   **Where:** `src/App.cpp:871`
- **What you see:** The obvious next move after watching the default event is to pick another scenario and press Spawn star again (the old event stays active until Clear). When that happens the camera cuts to the new star, and on the next frame the star is already gas at its release point, so the 'Falling in' ride-along never happens. With 'Sun-like star vs Gargantua (swallowed whole)' the star is swallowed inside the spawn frame and never drawn: the camera jumps to an empty spot 12 M from the hole and nothing happens. A first spawn behaves the same way if the world time warp is high (for example the '1 yr/s' button gives about 1,000 M per frame).
- **Why:** updateWorld works out dtEvent from tdeWarp_ and sPerMNow before updateTde runs (App.cpp:912-917). updateTde then spawns the new star (796-815, which can also change the mass at 799) and recomputes tdeWarp_ for it (855-867). But tde_.step(dtM) at 871 still advances it with the dtM built from the previous event's warp and the previous mass. After a release the warp grows by 0.35*timeM per second with no end (864). By the old event's fallback (timeM >= t_min, about 3.4e5 M for Sun/Sgr A*) that is about 1.2e5 M/s, roughly 2,000 M in one 60 fps frame. The new star's whole approach (3 r_t to pericentre) is only about 220 M, and a swallowed star's plunge from rStart = 12 M (TdePhysics.h:364) is about 30 M. TdeSim's rigid phase is not cost-capped: advanceRigid integrates the whole interval (TdeSim.cpp:684-711) and sets finished=true on reaching the horizon. buildBodies then finds no intact star, the UI gets tdeActive=false, and the watch is dropped on the next frame (App.cpp:972).
- **Suggested fix:** Do not step a freshly spawned event with the old interval. Reset tdeWarp_ = 0 on spawn and pass the real frame dt into updateTde. After the spawn and auto-warp update, compute dtM from the new event's own warp and the new mass: dtM = (tdeWarp_ > 0 ? tdeWarp_ : timeWarp) * dt / secondsPerM(settings_.bhMassSolar). Simpler still: skip tde_.step on the spawn frame.

### The disruption camera overrides Travel, Go, Ride along, the bookmarks and R / 1-4 for the whole event

- **Severity:** high   **Status:** unchecked   **Where:** `src/App.cpp:1005`
- **What you see:** While a disruption plays, the Travel buttons (Black hole, planets, Star, galaxies), the planets' Go and Ride along, the Camera panel bookmarks and the R / 1-4 keys seem to do nothing. During the ride-along the camera snaps straight back to the star. After the pull-back, which lasts until Clear because events rarely end, the flight finishes and then the camera is dragged back to the watch distance. In 'Red supergiant vs Gargantua', pressing R or Travel > Black hole arrives at 25-30 M and is pulled back out to about 1,750 M within about 3 s, where the hole is a ~5 px dot (the user's 'zoomed out so far' confusion). A flight to Miller is pulled away in the first frame, about 200 planet radii per frame, and the ride-along drops after 2 frames. A trip to a galaxy is undone within a few seconds. Only WASD or the mouse release the camera, and the panel never says so.
- **Why:** Phase 1 calls camera_.setPose(...) every frame (App.cpp:1005). setPose sets transitioning_ = false (Camera.cpp:74-78), which cancels any flight started by goTo (App.cpp:1070-1072 -> flyToTarget), by the R/1-4 keys (App.cpp:355-359 -> flyTo) or by the bookmark buttons (Ui.cpp:625). Ride along (Ui.cpp:378) is also overwritten by the next setPose. Phase 2 only waits for !camera_.isTransitioning() and then scales the position toward max(30, 5.5*watchPeriM_) every frame (App.cpp:1017-1025; f = (target/d)^(1.2 dt) is about 4% of the radius per frame at 60 fps). That undoes any user flight once it arrives and breaks the planet follow (400-radii cutoff, App.cpp:1034). The only thing that ends the watch is `steering` (WASD/mouse/roll, App.cpp:971-973).
- **Suggested fix:** Treat any flight or ride-along the user starts as taking over. Set watchPhase_ = 0 in goTo() and in the R/1-4 handlers. In phase 1, end the watch if camera_.isTransitioning() or followName_ is set (phase 1 never starts flights itself). In phase 2, set a flag when the watch starts its own flight (App.cpp:1014), and end the watch if a different flight or a follow starts. Add a line to the panel saying that flying or travelling takes over the camera.

### Watch with no event running parks the camera on the horizon, staring into the shadow (all-black screen)

- **Severity:** medium   **Status:** confirmed   **Where:** `src/App.cpp:816`
- **What you see:** Watch is always enabled. Pressing it before any star has been spawned, or after a star was swallowed whole and the event ended by itself, puts the camera 2.3 M from the hole looking straight in. The shadow then fills the whole screen, and free flight there creeps at about 0.1 M/s, so it looks broken. After Clear, Watch cuts instead to the empty spot where the last star was released (about 1,300 M out for the red supergiant). After pericentre it first cuts to the distant debris core and then flies back.
- **Why:** The Watch button sits outside the BeginDisabled block (Ui.cpp:445-451). updateTde handles watchRequested before it checks tde_.active() (App.cpp:816-838) and reads tde_.starPosition() and starRadiusM(). Before any spawn these are comX{0.0} and EventSetup R = 0 (TdeSim.cpp:194, TdePhysics.h:329), so the camera is set about 1e-5 M from the singularity and aimed at it. After a swallowed star, comX is where it crossed the horizon; after clear(), released=false and comX is the old release point. On the next frame the watch is dropped because !tde_.active() (App.cpp:972), and Camera::update's barrier pushes the camera out to kMinRadius = 2.3 M along the same line, still facing the hole (Camera.cpp:197-202). At r = 2.3 M the shadow's half-angle is about 55 degrees, wider than the 30-degree vertical and ~46-degree horizontal half field of view. Flight speed is 0.35 x nearest-surface distance (0.3 M) (App.cpp:1063-1064).
- **Suggested fix:** The suggested fix is right. The core needs two parts.

(1) Ui.cpp:450-451:
`ImGui::SameLine(); ImGui::BeginDisabled(!c.tdeActive); if (ImGui::Button("Watch")) t.watchRequested = true; ImGui::EndDisabled();`
Optionally add a tooltip such as "Spawn a star first" using IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled).

(2) App.cpp:816: always consume the flag, but only move the camera while an event exists:
`if (tdeCtl_.watchRequested) { tdeCtl_.watchRequested = false; if (tde_.active()) { ...existing body... } }`
Spawn sets watchRequested right after tde_.spawn(), when active() is already true, so the auto-watch on spawn is unaffected.

The pericentre part is optional polish, not required. If it is done, compute watchPeriM_ first. Then, when (st.released && dot(p, v) > 0 && |p| > 1.6 * watchPeriM_) or (st.swallowedWhole && |p| < 4), skip setPose and call `camera_.flyToTarget(glm::dvec3(0.0), std::max(30.0, 3.0 * watchPeriM_), normalize(camera_.position()))` with watchPhase_ = 2. This uses the same condition and D as the phase-1 exit at App.cpp:1011-1015, so the camera flies straight to the pulled-back view instead of cutting to the far debris first. (verifier's correction of: Disable Watch unless c.tdeActive, and ignore watchRequested in updateTde when !tde_.active(). When Watch is pressed after the debris has passed pericentre, go straight to the pulled-back view (watchPhase_ 2, flyToTarget to the hole) instead of setPose next to the far-off core.)
- **Verifier's notes:** I traced the code and the finding holds. Watch is always enabled: only "Spawn star" sits inside BeginDisabled(!t.moduleAvailable) (Ui.cpp:445-451). The Star disruption section is open by default, and ctx.tde is always set. In updateTde (App.cpp:816-838) the watchRequested block runs before the `if (!tde_.active()) return;` check and calls camera_.setPose/lookAt without any guard.

- **Before any spawn:** comX is {0} and ev.R is 0 (TdeSim.cpp:194, TdePhysics.h:329). lapse() at r <= 2 gives kMinLapse, so v = 0 and every fallback stays finite. The camera is placed about 1.4e-5 M from the origin, aimed at it.
- **Same frame:** the watch block drops watchPhase_ because !tde_.active() (App.cpp:972). Camera::update's barrier then moves the camera out along the same ray to kMinRadius = 2.3 M (Camera.cpp:197-202), so it still points straight at the hole.
- **What the user sees:** the renderer models a static observer. shadowAngularRadius gives PI - asin(...) below 3 M, about 125 degrees at 2.3 M. That is wider than the whole 60-degree vertical FOV (about 50 degrees to the corners at 16:9). Rays from there never reach the disk (inner edge 6 M) or the planets, so the screen is fully black. Flight speed is 0.35 x 1.0 x 0.3, about 0.1 M/s.
- **Swallowed-whole case:** the menu has an explicit "Sun-like star vs Gargantua (swallowed whole)" scenario. advanceRigid sets finished = true when the star crosses r = 2.05, so the event ends by itself. comX is left at about 2 M and 14R is about 0.07 M, so pressing Watch afterwards gives the same black 2.3 M view.
- **After Clear:** clear() resets released but not comX/ev, and comX is never advanced after release. Watch then jumps to the old release point, max(2 rt, ...), which is about 1,274 M for the red supergiant.
- **After pericentre:** the phase-1 block immediately starts flyToTarget, so the user sees a cut to the debris and then a fly-back, as described. That part is minor polish.

The user can recover with Travel > Black hole, a bookmark, or by backing out in the dark. Even so, a default-visible button that turns the screen black looks broken to a non-expert. Medium severity is fair.

### Spawn star silently swaps the user's black hole for the scenario's, and neither Clear nor quitting gives it back

- **Severity:** medium   **Status:** unchecked   **Where:** `src/App.cpp:799`
- **What you see:** The default scenario is preselected, so the first press of Spawn star switches the world's black hole from Gargantua (100 million suns) to Sagittarius A* (4.3 million). The Mass slider jumps. The planets orbit 23x faster at the same time warp (the effect the user complained about). Miller (6.5 M) suddenly shows 'Note: at this black hole mass, tides would tear it apart here.' Clear does not restore the old black hole, and the mass is saved at exit, so the next launch starts at Sgr A*: the worktree's settings.ini already holds bhMassSolar=4297000. Just browsing the Scenario dropdown does the same. And if the user changes the mass mid-event (mass buttons, or switching to Real universe, which sets Sgr A*), the running event keeps its old units while the panel's 'after spawn' time and event clock jump by the mass ratio.
- **Why:** App.cpp:799 sets settings_.bhMassSolar = applyScenario(...), and the Scenario combo does the same as soon as it is picked (Ui.cpp:416-417). Clear only calls tde_.clear() (App.cpp:792-795). saveSettings writes settings_ unchanged (App.cpp:759-762), and bhMassSolar is a saved field (Settings.cpp fields()). Planets advance dtSim / secondsPerM(mass) (App.cpp:916): 21 s per M at 4.297e6 suns against 492 s per M at 1e8. Miller's Roche limit at 4.297e6 suns is about 16 M (BlackHoleSystem.cpp:117-119), above its 6.5 M orbit, which triggers the note at Ui.cpp:391-398. Mid-event, the panel converts with secondsPerM(s.bhMassSolar) (Ui.cpp:458) and tdeWarp_ with the current mass (App.cpp:867), while TdeSim keeps the mass it was spawned with.
- **Suggested fix:** Do what userWarp_ used to do for the warp: remember the user's mass when a scenario's mass is applied, restore it on Clear and when the event ends, and have saveSettings write the user's mass. Drop the immediate apply in the Scenario combo; the preview already uses the scenario's mass. Show next to Spawn which black hole it will switch to. Keep the event's own spawn mass for its time readouts.

### When the event is frozen (Pause, or Auto time warp off) the disruption panel still says it is running

- **Severity:** low   **Status:** confirmed   **Where:** `src/Ui.cpp:467`
- **What you see:** With the simulation paused, Spawn star places a star that never moves while the disruption panel reads 'Falling in: the black hole is pulling the star in.' and 'event clock: 1 s = 2.4 h'. Pausing can come from the P key or from the Accretion disk section's 'Pause disk (P)' checkbox, whose label hides that it freezes the planets and the event too. An event in progress freezes the same way without notice; the only pause indicator is two sections up, in Black hole & time. With Auto time warp unticked, the event crawls at the world warp (1/492 M per second at Gargantua, so it looks stuck), yet the panel still says 'only the event runs faster'.
- **Why:** dtEvent = 0 when settings_.paused (App.cpp:913-914), but the spawn and the camera placement still run (App.cpp:796-838). The status block (Ui.cpp:456-474) prints the phase and c.tdeWarp with no paused state and shows the 'The planets keep your own time warp; only the event runs faster.' line unconditionally (474). The 'Pause disk (P)' checkbox writes the global s.paused (Ui.cpp:575). With autoWarp off, tdeWarp_ = 0 and dtEvent falls back to the world warp (App.cpp:868-869, 914).
- **Suggested fix:** All changes are in the Star disruption status block in src/Ui.cpp (lines 456-474). UiContext has no tdeWarp_ (c.tdeWarp already falls back to the world warp), so use s.paused, t.autoWarp and c.tdeWarp versus s.timeWarp:

- If s.paused: print the clock as "event clock: paused". Add an amber line: "Paused (P): the star and its debris are frozen. Press P to continue."
- Else if !t.autoWarp: add "Auto time warp is off: the event runs at your time warp (1 s = %s), so it can take hours to move. Tick Auto time warp to speed it up." Showing the real-time length of the fall-in is optional and not needed.
- Else: replace the line at Ui.cpp:474 with a neutral one: "The event runs on its own clock; the planets keep your time warp." Only say "faster" when c.tdeWarp > s.timeWarp. The line is wrong whenever the world warp is above the event warp, e.g. after the 1 h/s preset button.

Also rename the checkbox at Ui.cpp:575 to "Pause everything (P)", matching README. Show the note at Ui.cpp:339 even when the disk is off, and reword it to "Paused (P): the clock, planets, disk and star disruption are stopped." (verifier's correction of: In the Star disruption block, show 'Paused (P): the event is frozen' when s.paused, and say when Auto time warp is off that the event runs at the world warp (with the real-time length of the fall-in). Show the 'only the event runs faster' line only when tdeWarp_ > 0. Rename the checkbox to 'Pause everything (P)'.)
- **Verifier's notes:** The code confirms each part of the finding.

1) Pausing freezes the event. With settings_.paused, dtEvent is 0 (App.cpp:914), and TdeSim's step() returns at once for dt <= 0 (TdeSim.cpp:1124). Spawn and the camera placement still run inside updateTde (App.cpp:796-838), so the star is placed and filmed but never moves. Nothing unpauses on spawn: settings_.paused is only changed by the P key (App.cpp:361) and the checkbox (Ui.cpp:575).

2) The panel does not show the pause. updateTde still computes tdeWarp_ every frame while paused (App.cpp:855-867). So the Star disruption block shows "Falling in: the black hole is pulling the star in." and "event clock: 1 s = <non-zero>" (Ui.cpp:467-469). The only sign of the freeze in that block is "0 s after spawn" never changing. The pause notes ("[paused, P]" at Ui.cpp:325, and "Paused (P): the disk and the clock are stopped." at Ui.cpp:339) sit in the Black hole & time section, two sections up. Line 339 only appears when the disk is on, and says "the clock" without mentioning the event. The panel is AlwaysAutoResize (Ui.cpp:238), and in ImGui 1.92.9b (CMakeLists.txt) such a window is capped at screen height and scrolls. With the Planets and Star disruption sections open, a user at the Spawn button has likely scrolled the pause note out of view. This is exactly the user's "the star does nothing" complaint.

3) The checkbox label is misleading. "Pause disk (P)" (Ui.cpp:575) writes the global s.paused, which also freezes the planets and the event. README says "P pauses everything".

4) Unticking Auto time warp: tdeWarp_ becomes 0, the event falls back to the world warp (App.cpp:868-869 and 914), and ctx.tdeWarp shows the world warp (App.cpp:566). At the saved 7.6x warp with the default Sgr A* scenario (1 M = 21 s), the fall-in takes over an hour of real time, so the event looks stuck. Line 474 still says "only the event runs faster", without any condition.

The line is also wrong with Auto time warp on. The default Sgr A* event clock is about 9 min/s at spawn and about 40 s/s near pericentre. The one-click "1 h/s" or "1 day/s" world warp is faster than that, so the planets outrun the event while the panel says only the event runs faster. Gating the line on tdeWarp_ > 0, as suggested, would not fix that case.

Mitigating factors: the user has to press P or tick the checkbox themselves; P is ignored while a text field has focus (App.cpp:342); paused is not saved to settings.ini; and a pause note does exist elsewhere in the panel. Severity low is right, but the effect is real.

### The 'swallowed whole' messages blame the black hole and suggest Sagittarius A* even when the orbit depth (beta) is the cause

- **Severity:** low   **Status:** confirmed   **Where:** `src/Ui.cpp:465`
- **What you see:** Raising Penetration (beta) above about 4.5 on the default Sun-like / Sgr A* setup, or above about 3.6 in the white dwarf vs 10,000-sun scenario, makes the preview say 'Too compact for these tides' and the event say 'Swallowed whole: this black hole is too big to tear this star apart (try a red giant, or Sagittarius A*).' The user is already at Sagittarius A*, and the actual fix is to lower beta, so they are sent in the wrong direction.
- **Why:** The preview computes rp = rt / beta and, if rp < 4.0, shows 'Too compact for these tides...' (Ui.cpp:440-442). The phase-5 text (Ui.cpp:465) is fixed and appears whenever st.swallowedWhole, which is rp < kCaptureRadius = 4 M (TdePhysics.h:39, 362), whether or not rt itself is above 4 M. Sun-like at 4.297e6 suns has rt of about 17.8 M, so beta > 4.45 gives rp < 4; the white dwarf at 1e4 has rt of about 14.4 M.
- **Suggested fix:** The suggested split is right; four changes refine it.

(1) The phase-5 text describes the event that is running, so take its numbers from the event's own TdeStats (st.tidalRadiusM, st.pericenterM), not from the panel's current rt and t.beta. The user can move the sliders mid-event.

(2) Print the beta limit rounded down: betaOk = std::floor(rt / 4.0 * 100.0) / 100.0. With %.2g or %.3g, the red dwarf at Sgr A* (rt/4 = 1.997) would print 'below 2', and 2.0 is exactly the value that gets swallowed.

(3) When rt < 4 M, say 'try a bigger star (e.g. a red giant) or a lighter black hole'. Name Sagittarius A* only when bhMassSolar > 4.297e6, because for lighter holes it makes things worse.

(4) Apply the same split to the preview line at Ui.cpp:442.

Sketch for the preview:
if (rp < 4.0) {
  if (rt >= 4.0) TextColored(warm, "Dives too deep: it will be swallowed whole. Penetration below %.2f tears it apart.", betaOk);
  else TextColored(warm, "This black hole is too big for this star: it will be swallowed whole (try a red giant%s).", bhMass > 4.297e6 ? " or Sagittarius A*" : " or a lighter black hole");
}

At Ui.cpp:465, keep phases[0..4]. For c.tdePhase == 5, build the text from st.tidalRadiusM in the same way. If rt >= 4: 'Swallowed whole: the orbit dived inside 4 M before the tides could tear it apart (Penetration below %.2f tears this star apart here).' Otherwise use the 'too big' text with the mass-dependent suggestion. (verifier's correction of: If rt >= 4 M, say the orbit dives too deep and suggest beta below rt/4, giving the number. Keep 'black hole too big for this star (try a red giant or a lighter hole)' for rt < 4 M only, and drop the Sgr A* suggestion when the hole already has that mass.)
- **Verifier's notes:** The finding holds up against the code. phases[5] at Ui.cpp:465 is a fixed string. It shows whenever App::tdePhase() returns 5 (App.cpp:767), and that happens whenever st.swallowedWhole is set. swallowedWhole is ev.swallowed, which is just s.rp < kCaptureRadius (4 M) (TdePhysics.h:362, TdeSim.cpp:403). It does not depend on rt.

The Penetration slider is a plain log slider from 0.5 to 10 (sliderLogD, Ui.cpp:426) with no limit tied to rt. Recomputing with the Units.h constants:
- Sun-like star at 4.297e6 suns: rt = 17.82 M, so any beta above 4.456 is swallowed.
- White dwarf at 1e4 suns: rt = 14.44 M, so any beta above 3.61 is swallowed.

There is an even easier trigger that needs no slider. Pressing Spawn on the default Sgr A* scenario sets settings_.bhMassSolar to 4.297e6, and that value is saved (bhMassSolar is a Settings field). If the user then picks 'Red dwarf' in the Star combo, applyStarPreset leaves beta at 2.0 and the scenario becomes Custom on the current (Sgr A*) mass. That gives rt = 7.988 M and rp = 3.994 M, which is under 4, so the star is swallowed. The panel then says 'this black hole is too big to tear this star apart (try a red giant, or Sagittarius A*)' while the user is already on Sagittarius A*. Lowering beta to 1.9 would tear the star apart.

The preview line 'Too compact for these tides' (Ui.cpp:442) has the same flaw: it never mentions the orbit depth. The phase-5 text was added in f9cc2dc, so this is a bug in one of those fixes.

The 'try Sagittarius A*' advice is also wrong for holes lighter than Sgr A* where rt < 4 M. For example, a white dwarf at 1e5 suns has rt of about 3.1 M, and moving to Sgr A* makes it worse. Severity low is right: the simulation behaves as designed, and the 'red giant' suggestion does work. What misleads is the diagnosis and the Sgr A* advice.

### The event clock never stops speeding up, and the panel shows the requested pace even when the simulation cannot keep up (the cost-cap note was removed)

- **Severity:** low   **Status:** unchecked   **Where:** `src/App.cpp:864`
- **What you see:** A disruption left running keeps getting 35% faster every second, long after the debris has fallen back, contrary to the comment and README. Once the GPU cost cap is reached, the panel's 'event clock: 1 s = ...' keeps rising (up to the 1e11 clamp, '1 s = 3,169 yr') further and further from what the gas on screen is doing. f9cc2dc removed the line that used to say the simulation was running slower than the clock. The ever-growing warp also makes the first problem above worse.
- **Why:** perSecondM = max(tdyn/15, 0.35*timeM) has no end condition (App.cpp:864), even though the comment says 'until the debris falls back' (859-861). tdeWarp_ is only clamped at 1e11 (867). The UI prints c.tdeWarp = tdeWarp_ (App.cpp:566, Ui.cpp:468-469). TdeSim::step quietly advances less when over budget and sets stats().limited (TdeSim.h step()), but f9cc2dc deleted the `if (st.limited) ...(cost cap)` line from Ui.cpp.
- **Suggested fix:** Stop the ramp once the debris falls back: for example, freeze perSecondM at its value when accretedFraction first exceeds 1e-3, or cap it at a fixed fraction of the fallback time. Bring back the cost-cap note, or display the pace actually achieved (change in timeM per real second).

### 'Accumulate when still' never runs: the camera's moved flag is never cleared (outside the disruption flow, found while checking for gas ghosting)

- **Severity:** low   **Status:** unchecked   **Where:** `src/Camera.cpp:206`
- **What you see:** The Quality option 'Accumulate when still' has no effect. The image never supersamples when the camera stops, and the stats line always reads 'accum 1'.
- **Why:** Camera::update sets moved_ = moved_ || ... (Camera.cpp:206-209). Nothing ever sets it back to false: setPose, lookAt, shift, pushOutOf and markMoved only set it true, and it starts true. So App.cpp:506 fi.resetAccumulation = camera_.movedThisFrame() || settingsChanged is always true, and Renderer::render resets accumCount_ every frame (Renderer.cpp:618). This came in with c86dbd9, which changed `moved_ = transitioning_ || ...` to `moved_ = moved_ || ...`.
- **Suggested fix:** Add Camera::clearMoved() and call it in App::frame after fi.resetAccumulation has been read (a reset at the start of update would lose the watch's setPose earlier in the same frame). When fixing this, also limit accumulation while a disruption is active: the cap at Renderer.cpp:625 only looks at diskAnimating, so moving gas would ghost across up to 256 frames when the disk is off or not rotating.

## First launch, saved settings and docs

### Pressing Esc to close an open dropdown quits the whole app

- **Severity:** high   **Status:** confirmed   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/src/App.cpp:348`
- **What you see:** A user opens a dropdown such as Universe, Black hole placement, Scenario, Star, Preset or Tonemap, then presses Esc to dismiss it, as most apps allow. The window closes at once, which looks like a crash. This is exactly the 'pick or change the universe' flow the user just complained about. Settings are saved, but the running star disruption, the camera position and any edited planet orbits are lost.
- **Why:** App::onKey (src/App.cpp:345-349) ignores Esc only when ui_.wantsKeyboard(), which is io.WantCaptureKeyboard (src/Ui.cpp:114). In the bundled Dear ImGui 1.92.9b that flag is set only while an item is active or a modal window is open (imgui.cpp:5550-5557). An open Combo popup is neither once the click is released. Ui::init never sets ImGuiConfigFlags_NavEnableKeyboard, so ImGui does not close popups on Escape either (NavUpdateCancelRequest, imgui.cpp:14464-14466). A headless test built against the same ImGui sources (/tmp/bhs-review-esc/t.cpp) shows popupOpen=1, ActiveId=0 and WantCaptureKeyboard=0 while the 'Universe##mode' combo is open, and the popup is still open after Escape. Opening a dropdown requires a free mouse, so the key reaches `else quit_ = true;` (App.cpp:348) and the main loop exits (App.cpp:310-314).
- **Suggested fix:** Only check for an open popup, and close it at the start of the next UI frame. Do not also stop quitting whenever io.WantCaptureMouse is set: that is true whenever the free cursor is over the panel, so Esc would silently do nothing there, which goes against the panel's own "Esc: quit" hint. Do not turn on NavEnableKeyboard either. It would not stop the quit, because App::onKey runs before ImGui sees the key. It would also set WantCaptureKeyboard whenever the panel has focus, which blocks the H/P/L/1-4 shortcuts and Esc-to-quit.

Ui.h: add `bool popupOpen() const; void requestClosePopups() { closePopups_ = true; }` and a member `bool closePopups_ = false;`.

Ui.cpp: add `#include <imgui_internal.h>`, then
`bool Ui::popupOpen() const { return inited_ && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }`
(this is safe outside a frame: it only reads OpenPopupStack.Size). In Ui::newFrame, right after ImGui::NewFrame():
`if (closePopups_) { ImGui::ClosePopupsExceptModals(); closePopups_ = false; }`

App.cpp onKey, Esc case:
```
if (ui_.wantsKeyboard()) break;
if (mouseCaptured_) setMouseCaptured(false);
else if (ui_.popupOpen()) ui_.requestClosePopups();   // Esc first dismisses an open dropdown
else quit_ = true;
```
Optionally change the hint at Ui.cpp:267 to "Esc: close menu / quit". (verifier's correction of: Add Ui::popupOpen() (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) and, in the Esc case, close the popup instead of quitting (for example ImGui::ClosePopupsOverWindow(nullptr, false) at the next frame). Also consider not quitting while the cursor is over the panel (io.WantCaptureMouse).)
- **Verifier's notes:** The finding holds; I checked it in the code and with a headless test. Every dropdown in the panel is a plain ImGui::Combo (Ui.cpp 272, 280, 416, 421, 530, 536, 539, 557, 604). In ImGui 1.92.9b, BeginCombo uses ButtonBehavior's default PressedOnClickRelease mode, so the popup opens on mouse release and ClearActiveID() runs (imgui_widgets.cpp 563-564, 748-759). While the dropdown is open, ActiveId is 0 and the popup is not modal. io.WantCaptureKeyboard is only set when ActiveId is non-zero, a modal is open, or keyboard nav is enabled (imgui.cpp 5550-5558), and the app never sets ImGuiConfigFlags_NavEnableKeyboard (only NoMouse, Ui.cpp 98-99). ImGui itself only closes a popup on Esc when keyboard nav is on (NavUpdateCancelRequest, imgui.cpp 14464).

App::onKey is installed before ui_.init (App.cpp 211, 267), so ImGui chains it and calls it first, directly from glfwPollEvents (imgui_impl_glfw.cpp 485). The mouse has to be free to open a dropdown, because newFrame sets NoMouse while it is captured, and clicking on the panel does not capture it (App.cpp 376). So Esc reaches `else quit_ = true;` (App.cpp 348), the loop at App.cpp 310 exits and the window closes.

I built my own headless test against the same ImGui sources in /tmp/claude-verify-esc, using the "Universe##mode" combo inside a "Universe" header. After the click is released it shows popups=1, ActiveId=0, WantCaptureKeyboard=0. The popup is still open after Esc is pressed and released.

Only settings are saved (App.cpp 759-762), so the camera pose and a running star disruption are lost. One caveat: Esc quitting when the mouse is free is documented in README.md:23 and in the dim hint above the Universe header (Ui.cpp:267). Medium severity fits better than high. Still, Esc is the usual way to close a dropdown, and quitting the app at that moment looks like a crash.

### A star-disruption scenario (or a visit to the Real universe) permanently changes the black hole mass, and the change is saved

- **Severity:** medium   **Status:** unchecked   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/src/App.cpp:799`
- **What you see:** One click on 'Spawn star' with the default scenario sets the Interstellar scene to 4.3 million suns for the rest of the session and every later one. Miller then shows 'Note: at this black hole mass, tides would tear it apart here'. After the white-dwarf scenario, the next ./run.sh opens with a 10,000-sun hole: all three planets are flagged and drawn about 10,000x larger (Miller's radius is about a quarter of the horizon's). The user never touched the mass slider, and the README still says Gargantua (100 million suns) is the default. Just picking an entry in the Scenario dropdown already changes the world's mass, even in the middle of an event. This is the same kind of leak as the auto-warp bug that was just fixed.
- **Why:** Four paths overwrite settings_.bhMassSolar: the Scenario combo (src/Ui.cpp:416-417), Spawn star (src/App.cpp:799), --tde N (App.cpp:281) and entering the Real universe (App.cpp:787). Nothing restores the previous value on Clear (App.cpp:792-795), when the event ends (App.cpp:839-853), or when leaving the Real universe. App::saveSettings (App.cpp:759-762) writes settings_ unchanged at exit (App.cpp:314), and Settings::load restores it at the next launch. This worktree's settings.ini already holds universeMode=0 with bhMassSolar=4297000. Roche limits from BlackHoleSystem.cpp:117-120: at 4.297e6 suns Miller's limit is 16.4 M against its 6.5 M orbit. At 1e4 suns the limits are 934, 820 and 864 M for Miller, Mann and Edmunds (all inside), and Miller's radius is 0.542 M versus 5.4e-5 M at Gargantua.
- **Suggested fix:** Make the scenario mass event-only, like tdeWarp_. Remember the user's mass at Spawn and restore it on Clear or when the event ends. Do not change the mass when a scenario is merely selected (the preview already uses kTdeScenarios[i].bhMassSolar). Restore the pre-Real mass when leaving the Real universe. Have saveSettings write the user's own mass.

### 'Watch' with no event running parks the camera at the horizon facing the hole: black screen

- **Severity:** medium   **Status:** confirmed   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/src/App.cpp:816`
- **What you see:** Pressing Watch before any star has been spawned, or after a star was swallowed whole and the event ended, turns the whole view black. The panel reads '2.3 M from the hole' and flying crawls at about 0.1 M/s. Nothing on screen says why, and the user has to know to press R.
- **Why:** The Watch button (src/Ui.cpp:451) is never disabled. App::updateTde handles watchRequested (src/App.cpp:816-838) before the `if (!tde_.active()) return;` check at App.cpp:839. With no event, TdeSim reports its initial or stale state. Before any spawn, starPosition() returns comX = (0,0,0) (src/tde/TdeSim.cpp:194, 1165-1166) and starRadiusM() returns ev.R = 0 (src/tde/TdePhysics.h:329). So R becomes 1e-6, and the camera is placed about 1.4e-5 M from the singularity, aimed at it (App.cpp:821-834). Camera::update pushes it out to kMinRadius = 2.3 M along the same direction and keeps the orientation (src/Camera.cpp:197-204). On the next frame watchPhase_ is reset because no event is active (App.cpp:972-973), so the camera stays there. For a swallowed star the stale position is inside r = 2, with the same result. At r = 2.3 M the shadow covers about 125 degrees around the inward direction, so the whole field of view is black.
- **Suggested fix:** Combine the two suggestions and explain the disabled state. With the App-side guard alone, Watch silently does nothing, which is exactly the kind of complaint the user made.
(1) src/Ui.cpp:451: `ImGui::BeginDisabled(!c.tdeActive); if (ImGui::Button("Watch")) t.watchRequested = true; ImGui::EndDisabled(); ImGui::SetItemTooltip(c.tdeActive ? "Fly the camera to the star" : "Spawn a star first: there is nothing to watch yet");`
The tooltip shows on a disabled button with no extra flags. In ImGui 1.92.9b, style.HoverFlagsForTooltipMouse includes ImGuiHoveredFlags_AllowWhenDisabled.
(2) src/App.cpp:816, defence in depth: `if (tdeCtl_.watchRequested) { tdeCtl_.watchRequested = false; if (tde_.active()) { ...existing placement... } }`
The spawn path still works with this guard: it sets watchRequested right after tde_.spawn(), and spawn() sets spawned = true, so active() is already true. (verifier's correction of: Wrap the Watch button in ImGui::BeginDisabled(!c.tdeActive), or clear watchRequested without moving the camera when !tde_.active().)
- **Verifier's notes:** Confirmed by reading the code. The Watch button at src/Ui.cpp:451 is always shown and never disabled. Only Spawn star is inside BeginDisabled(!t.moduleAvailable). App::updateTde handles watchRequested at App.cpp:816-838, before the `if (!tde_.active()) return;` check at :839.

Before any spawn, the TdeSim::Impl defaults are comX = comV = 0 (TdeSim.cpp:194) and ev.R = 0 (TdePhysics.h:329). starPosition() returns comX because released is false. starVelocity() is 0 * lapse = 0, which gives kMinLapse at r = 0, so no NaN.

That makes R = 1e-6, n fall back to (0,1,0) and out fall back to (1,0,0). setPose puts the camera about 1.4e-5 M along normalize(1, 0.45, 0.35), and lookAt(mix(0,0,0.15) = origin) aims it straight at the hole. In the same frame, updateWorld resets watchPhase_ to 0 because the event is not active (App.cpp:972-973). camera_.update (App.cpp:1066) then applies the barrier at Camera.cpp:197-204, which pushes the position out to kMinRadius = 2.3 along the same ray and leaves the orientation alone. Nothing afterwards moves it, so the frame renders from r = 2.3 looking exactly at the centre.

The renderer is a static Schwarzschild observer. shadowAngularRadius in geodesic.glsl:56-59 gives pi - asin(sqrt(27)/2.3 * sqrt(1 - 2/2.3)) = 125.3 deg. With fovDeg = 60, even the screen corners at about 50 deg off-axis are inside the shadow. The disk (inner edge 6 M) cannot appear in the view: rays in the shadow cone never leave r < 3. The panel reads "2.3 M from the hole". Flying starts at 0.35 * (2.3 - 2) = 0.1 M/s (App.cpp:613, 1064).

Clear does not reset comX or ev (TdeSim.cpp:348-374). A star swallowed whole sets finished = true once it crosses r = 2.05 (advanceRigid, TdeSim.cpp:697-703), so its stale comX is at the horizon or inside it, and Watch afterwards gives the same black view. After Clear on a normal event, Watch jumps to the stale release point: not black, but meaningless.

One correction to the finding: the user is not limited to pressing R. The 1-4 keys, the Bookmarks buttons in the Camera section and Travel's "Black hole" button all recover the view. Still, nothing tells the user why pressing an innocent-looking enabled button turned the screen black. The finding is real and medium severity is fair.

### During a star disruption, Travel buttons, R and bookmarks 1-4 are overridden by the event camera (README says you can take over at any time)

- **Severity:** medium   **Status:** unchecked   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/src/App.cpp:971`
- **What you see:** While the star falls in, clicking Black hole, Miller or a star in Travel, or pressing R or 1-4, does nothing. Later in the event, the camera flies to the target and then slides back out to the hold distance (about 49 M for the Sgr A* scenario, about 377 M for 'Red giant vs Gargantua', about 1750 M for the supergiant). This lasts for the whole event, until Clear. A user who freed the mouse to use the panel gets no hint that only a movement key or mouse-look ends the event camera. README.md:175-176 says 'Fly or look around at any time to take over.'
- **Why:** The event camera gives up control only on `steering`: move or roll keys, or mouse-look while the mouse is captured (src/App.cpp:971-973). In phase 1 it calls camera_.setPose every frame (App.cpp:1005), and setPose clears any running flight (src/Camera.cpp:74-80). So a Travel flight (goTo -> flyToTarget, App.cpp:703-704 and 1070-1072) or a bookmark flight (App.cpp:355-359) is cancelled on the next frame before it moves. In phase 2, whenever no flight is running, the camera is rescaled toward max(30, 5.5 x pericentre) M every frame (App.cpp:1017-1025). That drags it back after a flight to Miller (6.5 M) or the photon-sphere bookmark (4.8 M) has arrived.
- **Suggested fix:** Set watchPhase_ = 0 whenever a Travel request, R, 1-4 or the auto-orbit key starts a camera action (in goTo and onKey). Show a short panel line while the event camera is active, for example 'Camera follows the event: move or use Travel to take over'.

### Switching the Real universe's black hole placement keeps the old mass (Gargantua with 4.3 million suns, or Sgr A* with 100 million)

- **Severity:** low   **Status:** confirmed   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/src/App.cpp:778`
- **What you see:** Switching 'Black hole' from 'Sagittarius A*' to 'Near the Sun (fictional Gargantua)' gives a Gargantua of 4.3 million suns, so Miller gets the tidal note and the TDE Custom preview uses the wrong hole. Switching the other way gives a 'Sagittarius A*' of 100 million suns. A fresh launch with the same placement shows a different mass, and the README says Sgr A* has its real mass of 4.3 million suns (README.md:132-133).
- **Why:** suggestedMassSolar is applied only when `enteringReal = mode == 2 && uniModeApplied_ != 2` (src/App.cpp:778, 787). Changing the 'Black hole##placement' combo (src/Ui.cpp:280) re-runs configure while uniModeApplied_ is already 2, so the previous placement's mass (4.297e6 or 1e8, src/universe/RealUniverse.cpp:944-959) stays. Separately, a saved Real universe resets the mass at every launch, after --mass was applied (App.cpp:250), so --mass is ignored in that case.
- **Suggested fix:** In App::applyUniverseSettings, compute this before the uniPlacementApplied_ assignment: `const bool newRealHole = mode == 2 && (uniModeApplied_ != 2 || settings_.realPlacement != uniPlacementApplied_);`. Then use it in place of enteringReal at line 787. To make --mass win, re-apply `if (opt.mass > 0.0) settings_.bhMassSolar = opt.mass;` right after the startup applyUniverseSettings() call (App.cpp:277), the same way --tde is already applied after it. Related, and also worth fixing: the galaxy-download reload (App.cpp:904-906) forces a rebuild by setting uniModeApplied_ = -1. That counts as 'entering Real', so when the download finishes the mass snaps back to the suggested value, even in the middle of a star-disruption scenario that set its own mass. Force that rebuild with a separate reload flag (checked in the early return) instead of -1, so it doesn't count as a new hole. (verifier's correction of: Also apply suggestedMassSolar when the placement changes (realPlacement != uniPlacementApplied_), or show the mass next to each placement entry.)
- **Verifier's notes:** The finding holds up when I trace the code. In App::applyUniverseSettings (src/App.cpp:773-789), `enteringReal = mode == 2 && uniModeApplied_ != 2` is only true when the app moves into the Real universe from a different mode (or at startup, when uniModeApplied_ is -1). Changing the 'Black hole##placement' combo (src/Ui.cpp:280, return value ignored) while already in mode 2 gets past the early return because the placement differs, so configure() rebuilds the universe with the new hole. But enteringReal is false, so suggestedMassSolar (4.297e6 for Sgr A*, 1e8 for Gargantua, RealUniverse.cpp:946/958) is never applied. A grep finds no other code that sets bhMassSolar on a placement change: the only writers are the --mass flag, --tde, line 787, the scenario spawn and the UI mass buttons/combo.

What the user sees: they go Real (the mass becomes 4.297e6, Sgr A*) and then pick 'Near the Sun (fictional Gargantua)'. 'Black hole & time' still shows 4.297 million suns, right next to the 'Gargantua (100 M)' button. Miller's Roche limit at 4.297e6 works out to about 16.4 M, against its 6.5 M orbit, so 'Note: at this black hole mass, tides would tear it apart here.' stays on. A fresh Gargantua at 1e8 has a limit of about 2.0 M and shows no note. Planet years and real-time speeds are about 23x off as well. Going the other way gives 'Sagittarius A*' at 1e8, which contradicts README.md:132-133 ('with its real mass, 4.3 million suns'). With the Custom scenario, a Sun-like star is then swallowed whole, and the hint says 'try ... Sagittarius A*' even though the user is already there.

The settings file saves realPlacement and the stale mass. On the next launch uniModeApplied_ is -1, so enteringReal is true and the mass jumps to that placement's own value: the same placement shows a different mass after a restart. The --mass claim is also correct. Line 250 sets the mass, then the startup applyUniverseSettings() (line 277 to line 787) overwrites it whenever the universe is Real. --tde is applied after that call, so --tde is the one that wins.

Caveats:
- The hole itself looks the same at any mass (README), so only the numbers, notes and planet scale give it away.
- The Custom preview is consistent with the mass that will actually be used. It just isn't Gargantua's mass.

Low severity is about right. The suggested fix is correct as long as the check is computed before uniPlacementApplied_ is overwritten.

### Galaxy download: the failure reason is cut off and never logged, and running the script by hand is not picked up

- **Severity:** low   **Status:** unchecked   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/src/CatalogFetch.h:41`
- **What you see:** When the download fails, which is most likely when offline, the panel shows 'Download failed:   https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/CURRENT/hygdata_v41.csv: <urlopen error [Errno -...' with the actual reason cut off. Nothing is printed in the terminal either, so the reason is visible nowhere. If the user instead follows the README's alternative and runs `python3 tools/fetch_real_catalogs.py` while the app is open, the panel keeps saying 'No galaxy catalogue yet' until the universe is switched away and back or the app restarts. When python3 is missing the message is 'sh: 1: python3: not found', which is acceptable.
- **Why:** The fetch thread keeps only the last non-empty output line and truncates it to 117 characters (src/CatalogFetch.h:36-42). App::updateWorld logs only the start and a success (src/App.cpp:899-907). On failure the script's last output line is `  <url>: <reason>` from the RuntimeError list (tools/fetch_real_catalogs.py:290, 295, 1969), so the reason sits at the end of a long line. The HYG-mirror line is 157 characters, and the VizieR query URLs alone are longer than 117. The universe reloads only when the mode or placement changes or an in-app download reports Done (App.cpp:775-777, 904-907), and nothing watches assets/real/galaxies.bin. The status text points users at the manual command (src/UniverseScene.cpp:411-413, README.md:136-137).
- **Suggested fix:** Log every script output line with logInfo and keep the last few. On failure, show the error trimmed from the left, or the last 'failed:' line, instead of cutting its end. Reload the Real universe when galaxies.bin appears or its modification time changes, or add a 'Reload' button next to the status.

### One-off command-line flags (--warp, --disk none, --lensing 0, --mass, --tde N...) are saved and become the default for every later ./run.sh

- **Severity:** low   **Status:** unchecked   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/src/App.cpp:251`
- **What you see:** Trying a README option once changes every later plain launch. `./run.sh --warp 100000` makes the planets race on every later ./run.sh (the user's original complaint). `--disk none` starts every later session with no accretion disk. `--lensing 0` makes the hole look unlensed. `--tde 5` leaves a 10,000-sun hole. The README's Command line section (README.md:210-232) presents these as launch options, with no mention that they persist.
- **Why:** run() copies --preset, --seed, --vsync, --lensing, --disk, --doppler, --exposure, --mass, --warp, --universe, --universe-seed, --fov, --placement, and the scenario mass from --tde, into settings_ (src/App.cpp:241-255, 281). Every interactive run then saves settings_ at exit (App.cpp:314). Only showUi is protected from this (App.cpp:244).
- **Suggested fix:** Remember the values loaded from settings.ini for the fields a flag overrode and save those instead, as is already done for showUi. Or document that flags persist.

### README sends users to the Quality panel for the gas brightness and opacity sliders, but they are in the Star disruption section

- **Severity:** low   **Status:** confirmed   **Where:** `/mnt/storage/BlackholeSimulation/.claude/worktrees/universe/README.md:181`
- **What you see:** A user following the README opens the Quality section and does not find the gas controls.
- **Why:** README.md:181 says '**Gas brightness and opacity** (Quality panel) scale the debris' glow and absorption.' The sliders are drawn at the end of the Star disruption section (src/Ui.cpp:476-477), and the Quality section (src/Ui.cpp:527-548) has no gas controls.
- **Suggested fix:** Edit only the README, using the same wording as the line before it (line 180 says "*Black hole & time* section"). README.md:181 becomes: "* **Gas brightness and opacity** (at the bottom of the *Star disruption* section) scale the debris' glow and absorption." Leave the sliders where they are in src/Ui.cpp. (verifier's correction of: Change the README to '(Star disruption panel)', or move the sliders into Quality.)
- **Verifier's notes:** The finding is correct. README.md:181 says "**Gas brightness and opacity** (Quality panel) scale the debris' glow and absorption." In the code, the only place these two sliders are drawn is src/Ui.cpp:476-477 (`sliderLog("Gas brightness", &s.gasBrightness, ...)` and `sliderLog("Gas opacity", &s.gasOpacity, ...)`). That is the last part of the "Star disruption" section, which starts at Ui.cpp:407. The "Quality" section (Ui.cpp:527-548) contains only Preset, Render scale, Max steps, Step scale, Disk step, Disk mode, Sky resolution, Dynamic resolution / Target ms, Accumulate when still and VSync.

A grep for gasBrightness and gasOpacity across src/ finds no other UI control: the remaining hits are Settings.h/.cpp (storage) and Renderer.cpp (uniforms). App.cpp:562 always sets `ctx.tde = &tdeCtl_`, so the Star disruption section is always drawn and open by default. The sliders sit outside the `if (c.tdeActive ...)` block, so they are always visible there and never in Quality.

This is not a regression from f9cc2dc. In c1937bc, the commit that added the README line, the sliders were already in the Star disruption section (Ui.cpp:412-413 at that commit), so the README was wrong from the start. The user impact is real but small: someone following the README opens the collapsed Quality section and doesn't find the controls. They could still find them by chance at the bottom of the Star disruption section, which is open by default.

Of the two suggested fixes, editing the README is the right one. Moving the sliders into Quality would be worse: they only affect the star-disruption debris, they belong next to the Spawn/Clear controls, and the README would still need editing.
