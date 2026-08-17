// The whole milestone in one component: drop it on an empty GameObject and
// press Play. It builds spheres for bodies, dots for spacecraft and line
// renderers for orbits, then advances the sim with Unity's frame clock —
// the same pacer-behind-the-boundary shape as the terminal app's loop.
//
// Keys: -/= warp   tab focus   L launch   ./, prograde/retrograde burn
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

        /// Beyond this many units from the focus, renderers switch off. Kept
        /// inside Unity's floating-point comfort zone (the editor warns near
        /// 100k units), which also means the Sun is culled from Earth focus —
        /// at true scale it is sub-pixel there anyway. Tab to the Sun to see
        /// it framed properly.
        private const float MaxRenderUnits = 40000f;

        /// One colour per body, matched to the terminal palette's hues, so the
        /// scene view reads at a glance. Orbit lines carry the same colour,
        /// dimmed.
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

        private static bool Finite(Vector3 v) =>
            float.IsFinite(v.x) && float.IsFinite(v.y) && float.IsFinite(v.z);

        /// Last range-verdict per object, so the renderer is only touched when
        /// OUR verdict changes — toggling a renderer by hand in the Inspector
        /// must stick, not be re-asserted sixty times a second.
        private readonly Dictionary<Transform, bool> _rangeVerdict =
            new Dictionary<Transform, bool>();

        /// Assign a position only when it is sane; a hidden renderer beats a
        /// console full of NaN/AABB errors.
        private void Place(Transform t, Vector3 position)
        {
            var visible = Finite(position) && position.magnitude < MaxRenderUnits;
            if (!_rangeVerdict.TryGetValue(t, out var previous) || previous != visible)
            {
                _rangeVerdict[t] = visible;
                var renderer = t.GetComponent<Renderer>();
                if (renderer != null)
                {
                    renderer.enabled = visible;
                }
            }
            if (visible)
            {
                t.localPosition = position;
            }
        }

        /// Frame the focused body the way the terminal camera does: distance
        /// proportional to its radius, so tabbing from Earth to the Moon to
        /// the Sun always lands somewhere you can see.
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
            camera.farClipPlane = Mathf.Max(1000f, distance * 50f);
        }

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
                    // The Sun is the light source: it should glow, not be lit.
                    material.EnableKeyword("_EMISSION");
                    material.SetColor("_EmissionColor", colour * 1.5f);
                }

                _bodies.Add(sphere);
                _bodyOrbits.Add(MakeOrbitLine($"{sphere.name} orbit", colour * 0.55f));
            }
            SyncCraftObjects();
            FrameFocus();
            LogRoster();
        }

        /// One line per body at startup: colour, distance from the focus, and
        /// whether it is inside the render bubble. Verification data for a
        /// human scanning the console — or a tool reading it over the bridge.
        private void LogRoster()
        {
            var focus = _sim.BodyState(_focusBody).position;
            var lines = $"omma roster (focus {_sim.BodyName(_focusBody)}, "
                      + $"1 unit = {_sim.MetresPerUnit / 1000.0:F0} km, "
                      + $"render bubble {MaxRenderUnits:F0} units)";
            for (int i = 0; i < _sim.BodyCount; ++i)
            {
                var p = _sim.ToUnity(_sim.BodyState(i).position, focus);
                var c = BodyColour(i);
                lines += $"\n  {_sim.BodyName(i),-8} rgb({c.r:F2},{c.g:F2},{c.b:F2})"
                       + $"  {p.magnitude,12:F1} units  "
                       + (p.magnitude < MaxRenderUnits ? "visible" : "culled (tab to visit)");
            }
            Debug.Log(lines);
        }

        private void OnDestroy() => _sim?.Dispose();

        private void Update()
        {
            HandleKeys();

            _sim.Advance(Time.deltaTime, WarpLadder[warpIndex]);
            SyncCraftObjects();

            var focus = _sim.BodyState(_focusBody).position;

            for (int i = 0; i < _bodies.Count; ++i)
            {
                var state = _sim.BodyState(i);
                Place(_bodies[i], _sim.ToUnity(state.position, focus));
                var visual = Mathf.Max(minimumVisualRadius,
                                       (float)(state.radius / metresPerUnit));
                _bodies[i].localScale = Vector3.one * visual * 2f;
                DrawOrbit(_bodyOrbits[i], _sim.BodyOrbit(i, _orbitBuffer, OrbitSamples), focus);
            }

            for (int i = 0; i < _craft.Count; ++i)
            {
                var state = _sim.CraftState(i);
                Place(_craft[i], _sim.ToUnity(state.position, focus));
                DrawOrbit(_craftOrbits[i], _sim.CraftOrbit(i, _orbitBuffer, OrbitSamples), focus);

                var renderer = _craft[i].GetComponent<Renderer>();
                renderer.material.color = state.alive == 0 ? new Color(0.4f, 0.3f, 0.3f)
                                        : state.burning != 0 ? new Color(1f, 0.7f, 0.3f)
                                                             : new Color(0.4f, 1f, 0.65f);
            }
        }

        private void HandleKeys()
        {
            if (Input.GetKeyDown(KeyCode.Equals) && warpIndex + 1 < WarpLadder.Length) ++warpIndex;
            if (Input.GetKeyDown(KeyCode.Minus) && warpIndex > 0) --warpIndex;
            if (Input.GetKeyDown(KeyCode.Tab))
            {
                _focusBody = (_focusBody + 1) % _sim.BodyCount;
                FrameFocus();
            }

            if (Input.GetKeyDown(KeyCode.L))
            {
                var request = new OmmaLaunchRequest
                {
                    name = $"U-{_sim.CraftCount + 1}",
                    aroundBodyIndex = _focusBody == 0 ? 3 : _focusBody,
                    altitudeMetres = 400e3,
                    inclinationRad = 0.9,
                    meanAnomalyRad = 0.6 * _sim.CraftCount,
                    dryMassKg = 200, propellantKg = 80,
                    maxThrustNewtons = 400, exhaustVelocityMps = 2200,
                };
                var id = _sim.Launch(ref request);
                Debug.Log(id != 0
                    ? $"launched {request.name} around {_sim.BodyName(request.aroundBodyIndex)}"
                    : $"launch refused around {_sim.BodyName(request.aroundBodyIndex)}");
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
            // An orbit that leaves the render bubble is dropped whole: a
            // partially-drawn ring lies, and giant coordinates in a
            // LineRenderer are where the AABB errors come from.
            for (int i = 0; i < points; ++i)
            {
                var p = _sim.ToUnity(_orbitBuffer[i * 3], _orbitBuffer[i * 3 + 1],
                                     _orbitBuffer[i * 3 + 2], focus);
                if (!Finite(p) || p.magnitude > MaxRenderUnits)
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

        private void OnGUI()
        {
            var status = $"warp {WarpLabels[warpIndex]}   focus {_sim.BodyName(_focusBody)}"
                       + $"   craft {_sim.CraftCount}"
                       + (_sim.FallingBehind ? "   [falling behind]" : "");
            GUI.Label(new Rect(12, 8, 900, 24), status);
            if (_sim.CraftCount > 0)
            {
                var e = _sim.CraftElements(_sim.CraftCount - 1);
                var s = _sim.CraftState(_sim.CraftCount - 1);
                var earthRadius = _sim.BodyState(3).radius;
                GUI.Label(new Rect(12, 30, 900, 24),
                    $"{_sim.CraftName(_sim.CraftCount - 1)}: "
                    + $"peri {(e.periapsisRadius - earthRadius) / 1000:F0} km  "
                    + $"apo {(e.apoapsisRadius - earthRadius) / 1000:F0} km  "
                    + $"dv used {s.deltaVSpentMps:F1} m/s  dv left {s.deltaVLeftMps:F0} m/s");
            }
            GUI.Label(new Rect(12, 52, 900, 24),
                      "-/=  warp    tab  focus    L  launch    . ,  burn");
        }
    }
}
