// The safe wrapper Unity code talks to. Owns the native handle, converts
// SI Z-up right-handed doubles into Unity's Y-up left-handed floats, and
// keeps every position FOCUS-RELATIVE: the solar system is 1e13 m across and
// a float loses metres at 1e11, so the camera's subject sits at the origin
// and everything else is measured from it — the same floating-origin trick
// as the terminal renderer's camera.
using System;
using System.Text;
using UnityEngine;

namespace Omma
{
    public sealed class OmmaSim : IDisposable
    {
        private IntPtr _sim;
        private readonly StringBuilder _nameBuffer = new StringBuilder(64);

        /// How many metres one Unity unit represents. 1e6 means Earth's
        /// radius is ~6.4 units and a low orbit ~7 — comfortable scene scale.
        public double MetresPerUnit = 1e6;

        public OmmaSim(string scenario, double startDaysSinceJ2000 = 9350.0)
        {
            if (OmmaNative.omma_abi_version() != 1)
                throw new InvalidOperationException("omma.dll ABI mismatch");
            _sim = OmmaNative.omma_create(scenario, startDaysSinceJ2000);
            if (_sim == IntPtr.Zero)
                throw new ArgumentException($"unknown scenario '{scenario}'");
        }

        public void Dispose()
        {
            if (_sim != IntPtr.Zero)
            {
                OmmaNative.omma_destroy(_sim);
                _sim = IntPtr.Zero;
            }
        }

        public long Advance(double frameDt, double warp) =>
            OmmaNative.omma_advance(_sim, frameDt, warp);

        public bool FallingBehind => OmmaNative.omma_falling_behind(_sim) != 0;
        public double NowSecondsSinceJ2000 => OmmaNative.omma_now_seconds_since_j2000(_sim);

        public int BodyCount => OmmaNative.omma_body_count(_sim);
        public int CraftCount => OmmaNative.omma_craft_count(_sim);

        public OmmaBodyState BodyState(int index)
        {
            OmmaNative.omma_body_state(_sim, index, out var state);
            return state;
        }

        public OmmaCraftState CraftState(int index)
        {
            OmmaNative.omma_craft_state(_sim, index, out var state);
            return state;
        }

        public OmmaElements CraftElements(int index)
        {
            OmmaNative.omma_craft_elements(_sim, index, out var elements);
            return elements;
        }

        public string BodyName(int index)
        {
            _nameBuffer.Clear();
            OmmaNative.omma_body_name(_sim, index, _nameBuffer, _nameBuffer.Capacity);
            return _nameBuffer.ToString();
        }

        public string CraftName(int index)
        {
            _nameBuffer.Clear();
            OmmaNative.omma_craft_name(_sim, index, _nameBuffer, _nameBuffer.Capacity);
            return _nameBuffer.ToString();
        }

        public long Launch(ref OmmaLaunchRequest request) =>
            OmmaNative.omma_launch(_sim, ref request);

        public long LaunchAscent(ref OmmaSurfaceLaunchRequest request) =>
            OmmaNative.omma_launch_ascent(_sim, ref request);

        public AscentPhase AscentPhaseOf(int index) =>
            (AscentPhase)OmmaNative.omma_craft_ascent_phase(_sim, index);

        public bool PlanTransfer(int fromBody, int toBody, double parkingRadius,
                                 out OmmaTransferPlan plan) =>
            OmmaNative.omma_plan_transfer(_sim, fromBody, toBody, parkingRadius,
                                          out plan) != 0;

        public long CraftId(int index) => OmmaNative.omma_craft_id(_sim, index);

        public bool CommandDeltaV(long craftId, BurnFrame frame, double deltaVMps) =>
            OmmaNative.omma_command_delta_v(_sim, craftId, (int)frame, deltaVMps) != 0;

        public void CancelBurn(long craftId) => OmmaNative.omma_cancel_burn(_sim, craftId);

        public int BodyOrbit(int index, double[] xyz, int count) =>
            OmmaNative.omma_body_orbit(_sim, index, xyz, count);

        public int CraftOrbit(int index, double[] xyz, int count) =>
            OmmaNative.omma_craft_orbit(_sim, index, xyz, count);

        /// Sim frame (Z-up, right-handed, metres) -> Unity (Y-up, left-handed,
        /// units), relative to a focus position. Swapping y and z flips the
        /// handedness and puts the ecliptic on Unity's ground plane.
        ///
        /// Guarded against a zeroed scale: an Inspector field wiped mid-play
        /// makes the focus body compute 0/0 and spray NaN transforms.
        public Vector3 ToUnity(OmmaVec3 p, OmmaVec3 focus)
        {
            return ToUnity(p.x, p.y, p.z, focus);
        }

        public Vector3 ToUnity(double x, double y, double z, OmmaVec3 focus)
        {
            var scale = MetresPerUnit > 0.0 ? MetresPerUnit : 1e6;
            return new Vector3(
                (float)((x - focus.x) / scale),
                (float)((z - focus.z) / scale),
                (float)((y - focus.y) / scale));
        }
    }
}
