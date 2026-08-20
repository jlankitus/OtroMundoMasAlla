// The Unity client, whole: local space + scaled-space sky, pad launches with
// a picture-in-picture launch camera, and Hohmann windows to Mars.
//
// The rendering architecture is the one real space software uses, in three
// layers (each defensible on its own):
//   1. TRUTH is double precision, in the C++ sim, in SI. Unity floats are
//      only ever a view of it.
//   2. FLOATING ORIGIN: everything is positioned relative to the focused
//      body, subtracted in doubles BEFORE narrowing to float. The camera's
//      subject always sits near (0,0,0) where float precision is thickest.
//   3. SCALED-SPACE SKY (the KSP/Orbiter trick): bodies too far for float
//      comfort are drawn on a fixed-radius shell in their TRUE direction at
//      their TRUE angular size (floored for findability). The near field is
//      to scale; the far field is a planetarium — honest about what a camera
//      would actually see, with no giant coordinates anywhere.
//
// Keys: -/= warp   tab focus   L (hold!) launch barrage   T Mars window
//       . , burn newest prograde/retrograde
using System.Collections.Generic;
using UnityEngine;

namespace Omma
{
    public sealed class SolarSystemView : MonoBehaviour
    {
        [Tooltip("empty, leo, constellation or transfer")]
        public string scenario = "leo";

        [Tooltip("Metres per Unity unit. 1e6: Earth radius ~6.4 units.")]
        public double metresPerUnit = 1e6;

        [Tooltip("Bodies smaller than this many units still render this big.")]
        public float minimumVisualRadius = 0.5f;

        [Tooltip("Index into the warp ladder below at startup.")]
        public int warpIndex = 3;

        private static readonly double[] WarpLadder =
            { 1, 60, 600, 3600, 21600, 86400, 604800 };
        private static readonly string[] WarpLabels =
            { "real time", "1 min/s", "10 min/s", "1 hour/s", "6 hours/s", "1 day/s", "1 week/s" };

        private const int OrbitSamples = 128;

        /// Local space ends here; beyond it the sky begins. Inside Unity's
        /// float comfort zone with room to spare.
        private const float MaxLocalUnits = 40000f;
        /// Radius of the scaled-space shell the far field is drawn on.
        private const float SkyShellUnits = 20000f;
        /// Minimum apparent radius on the shell: a planetarium exaggeration
        /// so planets are findable; at true angular size most are sub-pixel,
        /// exactly like the night sky.
        private const float MinSkyRadiusUnits = 60f;

        /// One colour per body, matched to the terminal palette's hues.
        private static readonly Color[] BodyColours =
        {
            new Color(1.00f, 0.93f, 0.55f),   // Sun
            new Color(0.55f, 0.50f, 0.45f),   // Mercury
            new Color(0.90f, 0.80f, 0.60f),   // Venus
            new Color(0.25f, 0.50f, 1.00f),   // Earth
            new Color(0.70f, 0.70f, 0.72f),   // Moon
            new Color(0.85f, 0.40f, 0.25f),   // Mars
            new Color(0.80f, 0.65f, 0.45f),   // Jupiter
            new Color(0.90f, 0.80f, 0.55f),   // Saturn
            new Color(0.55f, 0.85f, 0.90f),   // Uranus
            new Color(0.35f, 0.50f, 0.95f),   // Neptune
            new Color(0.75f, 0.65f, 0.60f),   // Pluto
        };

        private static Color BodyColour(int index) =>
            BodyColours[Mathf.Clamp(index, 0, BodyColours.Length - 1)];

        private OmmaSim _sim;
        private readonly List<Transform> _bodies = new List<Transform>();
        private readonly List<LineRenderer> _bodyOrbits = new List<LineRenderer>();
        private readonly List<Transform> _craft = new List<Transform>();
        private readonly List<LineRenderer> _craftOrbits = new List<LineRenderer>();
        private readonly double[] _orbitBuffer = new double[OrbitSamples * 3];
        private int _focusBody = 3;   // Earth
        private float _launchCooldown;
        private int _launchCount;

        private Camera _launchCamera;
        private RenderTexture _launchFeed;

        private static bool Finite(Vector3 v) =>
            float.IsFinite(v.x) && float.IsFinite(v.y) && float.IsFinite(v.z);

        // ── lifecycle ───────────────────────────────────────────────────────

        private void Start()
        {
            _sim = new OmmaSim(scenario) { MetresPerUnit = metresPerUnit };

            for (int i = 0; i < _sim.BodyCount; ++i)
            {
                var sphere = GameObject.CreatePrimitive(PrimitiveType.Sphere).transform;
                sphere.name = _sim.BodyName(i);
                sphere.SetParent(transform, false);

                var colour = BodyColour(i);
                var material = sphere.GetComponent<Renderer>().material;
                material.color = colour;
                if (i == 0)
                {
                    material.EnableKeyword("_EMISSION");
                    material.SetColor("_EmissionColor", colour * 1.5f);
                }

                _bodies.Add(sphere);
                _bodyOrbits.Add(MakeOrbitLine($"{sphere.name} orbit", colour * 0.55f));
            }
            SyncCraftObjects();
            FrameFocus();

            _launchFeed = new RenderTexture(384, 216, 16);
            _launchCamera = new GameObject("Launch Camera").AddComponent<Camera>();
            _launchCamera.transform.SetParent(transform, false);
            _launchCamera.targetTexture = _launchFeed;
            _launchCamera.nearClipPlane = 0.01f;
            _launchCamera.enabled = false;

            LogRoster();
        }

        private void OnDestroy()
        {
            _sim?.Dispose();
            if (_launchFeed != null)
            {
                _launchFeed.Release();
            }
        }

        // ── per frame ───────────────────────────────────────────────────────

        private void Update()
        {
            HandleKeys();

            _sim.Advance(Time.deltaTime, WarpLadder[warpIndex]);
            SyncCraftObjects();

            var focus = _sim.BodyState(_focusBody).position;

            for (int i = 0; i < _bodies.Count; ++i)
            {
                var state = _sim.BodyState(i);
                PlaceBody(i, _sim.ToUnity(state.position, focus), state);
                DrawOrbit(_bodyOrbits[i], _sim.BodyOrbit(i, _orbitBuffer, OrbitSamples), focus);
            }

            for (int i = 0; i < _craft.Count; ++i)
            {
                var state = _sim.CraftState(i);
                PlaceCraft(_craft[i], _sim.ToUnity(state.position, focus));
                DrawOrbit(_craftOrbits[i], _sim.CraftOrbit(i, _orbitBuffer, OrbitSamples), focus);

                var renderer = _craft[i].GetComponent<Renderer>();
                renderer.material.color = state.alive == 0 ? new Color(0.4f, 0.3f, 0.3f)
                                        : state.burning != 0 ? new Color(1f, 0.7f, 0.3f)
                                                             : new Color(0.4f, 1f, 0.65f);
            }

            UpdateLaunchCamera(focus);
        }

        // ── placement: local space, or the sky ──────────────────────────────

        /// Bodies never lie and never disappear: inside local space they sit
        /// at their true position at true scale; beyond it they move to the
        /// scaled-space shell — true direction, true (floored) angular size.
        private void PlaceBody(int index, Vector3 localPosition, OmmaBodyState state)
        {
            var t = _bodies[index];
            if (!Finite(localPosition))
            {
                return;   // keep the last honest value, never write garbage
            }

            var distance = localPosition.magnitude;
            if (distance < MaxLocalUnits)
            {
                t.localPosition = localPosition;
                var visual = Mathf.Max(minimumVisualRadius,
                                       (float)(state.radius / metresPerUnit));
                t.localScale = Vector3.one * visual * 2f;
            }
            else
            {
                // The sky: direction is exact; apparent radius is the true
                // angular size projected onto the shell, floored so the eye
                // can find it.
                t.localPosition = localPosition / distance * SkyShellUnits;
                var angular = (float)(state.radius / metresPerUnit) / distance;
                var apparent = Mathf.Max(MinSkyRadiusUnits, angular * SkyShellUnits);
                t.localScale = Vector3.one * apparent * 2f;
            }
        }

        /// Craft have no sky mode — a satellite around another planet is
        /// genuinely invisible from here. Hidden, at its true position.
        private void PlaceCraft(Transform t, Vector3 localPosition)
        {
            if (!Finite(localPosition))
            {
                return;
            }
            t.localPosition = localPosition;
            var visible = localPosition.magnitude < MaxLocalUnits;
            var renderer = t.GetComponent<Renderer>();
            if (renderer.enabled != visible)
            {
                renderer.enabled = visible;
            }
        }

        // ── input ───────────────────────────────────────────────────────────

        private void HandleKeys()
        {
            if (Input.GetKeyDown(KeyCode.Equals) && warpIndex + 1 < WarpLadder.Length) ++warpIndex;
            if (Input.GetKeyDown(KeyCode.Minus) && warpIndex > 0) --warpIndex;
            if (Input.GetKeyDown(KeyCode.Tab))
            {
                _focusBody = (_focusBody + 1) % _sim.BodyCount;
                FrameFocus();
            }

            // HOLD L for a barrage: five pads a second, spread in longitude
            // around the planet so the fleet fans out instead of stacking.
            // Every launch is Earth-based and starts ON THE PAD — watch the
            // picture-in-picture feed fly the gravity turn.
            _launchCooldown -= Time.deltaTime;
            if (Input.GetKey(KeyCode.L) && _launchCooldown <= 0f)
            {
                _launchCooldown = 0.2f;
                ++_launchCount;
                var request = new OmmaSurfaceLaunchRequest
                {
                    name = $"U-{_launchCount}",
                    bodyIndex = 3,
                    latitudeRad = 0.4974,                       // the Cape
                    longitudeRad = -1.4075 + 0.35 * (_launchCount % 18),
                    targetAltitudeMetres = 200e3 + 15e3 * (_launchCount % 8),
                    azimuthRad = 1.5708,
                };
                var id = _sim.LaunchAscent(ref request);
                Debug.Log(id != 0 ? $"pad launch {request.name}" : "launch refused");
            }

            // T: the next Mars window for the newest craft, and the injection
            // burn commanded NOW. Honest label: burn at the window (warp to
            // it first) for a real intercept; burn today and you get the
            // right-sized ellipse pointed the wrong way — which is itself a
            // lesson in why windows exist.
            if (Input.GetKeyDown(KeyCode.T) && _sim.CraftCount > 0)
            {
                if (_sim.PlanTransfer(3, 5, 6.771e6, out var plan))
                {
                    Debug.Log($"MARS WINDOW: opens in {plan.waitSeconds / 86400.0:F0} days, "
                            + $"transit {plan.transferSeconds / 86400.0:F0} days, "
                            + $"injection dv {plan.departureDeltaVMps:F0} m/s, "
                            + $"phase now {plan.currentPhaseAngleDeg:F1} deg "
                            + $"(need {plan.phaseAngleDeg:F1})");
                    var id = _sim.CraftId(_sim.CraftCount - 1);
                    if (_sim.CommandDeltaV(id, BurnFrame.Prograde, plan.departureDeltaVMps))
                    {
                        Debug.Log("injection burn commanded (prograde, now)");
                    }
                    else
                    {
                        Debug.Log("newest craft cannot afford the injection burn");
                    }
                }
            }

            if (_sim.CraftCount > 0)
            {
                var id = _sim.CraftId(_sim.CraftCount - 1);
                if (Input.GetKeyDown(KeyCode.Period))
                    _sim.CommandDeltaV(id, BurnFrame.Prograde, 10.0);
                if (Input.GetKeyDown(KeyCode.Comma))
                    _sim.CommandDeltaV(id, BurnFrame.Retrograde, 10.0);
            }
        }

        // ── cameras ─────────────────────────────────────────────────────────

        /// Frame the focused body by its radius, with the near/far planes set
        /// from the same distance — a 700,000:1 depth ratio z-fights.
        private void FrameFocus()
        {
            var camera = Camera.main;
            if (camera == null)
            {
                return;
            }
            var radiusUnits = Mathf.Max(
                minimumVisualRadius,
                (float)(_sim.BodyState(_focusBody).radius / metresPerUnit));
            var distance = Mathf.Max(30f, radiusUnits * 6f);
            camera.transform.position = new Vector3(0f, distance * 0.55f, -distance);
            camera.transform.LookAt(Vector3.zero);
            camera.nearClipPlane = Mathf.Max(0.05f, distance / 2000f);
            camera.farClipPlane = Mathf.Max(SkyShellUnits * 3f, distance * 50f);
        }

        /// The picture-in-picture launch feed: chase the newest ASCENDING
        /// craft while any exists; otherwise the feed is off.
        private void UpdateLaunchCamera(OmmaVec3 focus)
        {
            int ascending = -1;
            for (int i = _craft.Count - 1; i >= 0; --i)
            {
                var phase = _sim.AscentPhaseOf(i);
                if (phase >= AscentPhase.Vertical && phase <= AscentPhase.Circularize)
                {
                    ascending = i;
                    break;
                }
            }
            if (ascending < 0)
            {
                _launchCamera.enabled = false;
                return;
            }

            var craftPos = _sim.ToUnity(_sim.CraftState(ascending).position, focus);
            if (!Finite(craftPos) || craftPos.magnitude > MaxLocalUnits)
            {
                _launchCamera.enabled = false;
                return;
            }
            // Over the shoulder of the planet: sit slightly outward from the
            // craft (away from the focus body's centre) and look back at it.
            var outward = craftPos.magnitude > 0.001f ? craftPos.normalized : Vector3.up;
            _launchCamera.transform.position =
                craftPos + outward * 1.5f + Vector3.up * 0.4f;
            _launchCamera.transform.LookAt(craftPos, outward);
            _launchCamera.enabled = true;
        }

        // ── plumbing ────────────────────────────────────────────────────────

        private void SyncCraftObjects()
        {
            while (_craft.Count < _sim.CraftCount)
            {
                int i = _craft.Count;
                var dot = GameObject.CreatePrimitive(PrimitiveType.Sphere).transform;
                dot.name = _sim.CraftName(i);
                dot.SetParent(transform, false);
                dot.localScale = Vector3.one * 0.35f;
                _craft.Add(dot);
                _craftOrbits.Add(MakeOrbitLine($"{dot.name} path",
                                               new Color(0.35f, 0.95f, 0.7f)));
            }
        }

        private LineRenderer MakeOrbitLine(string name, Color colour)
        {
            var line = new GameObject(name).AddComponent<LineRenderer>();
            line.transform.SetParent(transform, false);
            line.loop = true;
            line.useWorldSpace = false;
            line.widthMultiplier = 0.05f;
            line.material = new Material(Shader.Find("Sprites/Default"));
            line.startColor = line.endColor = colour;
            line.positionCount = 0;
            return line;
        }

        private void DrawOrbit(LineRenderer line, int points, OmmaVec3 focus)
        {
            // An orbit that leaves local space is dropped whole: a partially
            // drawn ring lies, and giant LineRenderer coordinates are where
            // the AABB errors came from.
            for (int i = 0; i < points; ++i)
            {
                var p = _sim.ToUnity(_orbitBuffer[i * 3], _orbitBuffer[i * 3 + 1],
                                     _orbitBuffer[i * 3 + 2], focus);
                if (!Finite(p) || p.magnitude > MaxLocalUnits)
                {
                    line.positionCount = 0;
                    return;
                }
            }
            line.positionCount = points;
            for (int i = 0; i < points; ++i)
            {
                line.SetPosition(i, _sim.ToUnity(_orbitBuffer[i * 3],
                                                 _orbitBuffer[i * 3 + 1],
                                                 _orbitBuffer[i * 3 + 2], focus));
            }
        }

        /// One line per body at startup: colour, distance, local or sky.
        /// Verification data for a human scanning the console — or a tool
        /// reading it over the editor bridge.
        private void LogRoster()
        {
            var focus = _sim.BodyState(_focusBody).position;
            var lines = $"omma roster (focus {_sim.BodyName(_focusBody)}, "
                      + $"1 unit = {_sim.MetresPerUnit / 1000.0:F0} km, "
                      + $"local space {MaxLocalUnits:F0} units, sky shell {SkyShellUnits:F0})";
            for (int i = 0; i < _sim.BodyCount; ++i)
            {
                var p = _sim.ToUnity(_sim.BodyState(i).position, focus);
                var c = BodyColour(i);
                lines += $"\n  {_sim.BodyName(i),-8} rgb({c.r:F2},{c.g:F2},{c.b:F2})"
                       + $"  {p.magnitude,12:F1} units  "
                       + (p.magnitude < MaxLocalUnits ? "local" : "sky");
            }
            Debug.Log(lines);
        }

        // ── HUD ─────────────────────────────────────────────────────────────

        private void OnGUI()
        {
            var status = $"warp {WarpLabels[warpIndex]}   focus {_sim.BodyName(_focusBody)}"
                       + $"   craft {_sim.CraftCount}"
                       + (_sim.FallingBehind ? "   [falling behind]" : "");
            GUI.Label(new Rect(12, 8, 900, 24), status);

            if (_sim.CraftCount > 0)
            {
                int newest = _sim.CraftCount - 1;
                var e = _sim.CraftElements(newest);
                var s = _sim.CraftState(newest);
                var phase = _sim.AscentPhaseOf(newest);
                var earthRadius = _sim.BodyState(3).radius;
                var phaseText = phase != AscentPhase.None && phase != AscentPhase.Done
                    ? $"  ASCENT: {phase}" : "";
                GUI.Label(new Rect(12, 30, 900, 24),
                    $"{_sim.CraftName(newest)}: "
                    + $"peri {(e.periapsisRadius - earthRadius) / 1000:F0} km  "
                    + $"apo {(e.apoapsisRadius - earthRadius) / 1000:F0} km  "
                    + $"dv left {s.deltaVLeftMps:F0} m/s{phaseText}");
            }
            GUI.Label(new Rect(12, 52, 900, 24),
                      "-/=  warp    tab  focus    L (hold)  launch barrage    "
                      + "T  Mars window    . ,  burn");

            if (_launchCamera != null && _launchCamera.enabled)
            {
                var w = 384f;
                var h = 216f;
                var x = Screen.width - w - 12f;
                var y = Screen.height - h - 12f;
                GUI.DrawTexture(new Rect(x, y, w, h), _launchFeed, ScaleMode.StretchToFill);
                GUI.Box(new Rect(x, y - 22f, w, 22f), "LAUNCH CAM");
            }
        }
    }
}
