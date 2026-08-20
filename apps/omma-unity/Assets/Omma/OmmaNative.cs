// P/Invoke surface for omma.dll — a 1:1 mirror of src/ffi/omma_c.h.
// Everything is SI doubles in the sim's Z-up right-handed frame; OmmaSim.cs
// owns the conversion to Unity's Y-up left-handed floats.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Omma
{
    [StructLayout(LayoutKind.Sequential)]
    public struct OmmaVec3
    {
        public double x, y, z;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct OmmaBodyState
    {
        public OmmaVec3 position;
        public OmmaVec3 velocity;
        public double radius;
        public double gm;
        public int parentIndex;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct OmmaCraftState
    {
        public OmmaVec3 position;
        public OmmaVec3 velocity;
        public int centralBodyIndex;
        public int alive;
        public int burning;
        public double propellantKg;
        public double deltaVLeftMps;
        public double deltaVSpentMps;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct OmmaElements
    {
        public double semiMajorAxis;
        public double eccentricity;
        public double inclination;
        public double longitudeOfAscendingNode;
        public double argumentOfPeriapsis;
        public double periodSeconds;
        public double periapsisRadius;
        public double apoapsisRadius;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct OmmaLaunchRequest
    {
        [MarshalAs(UnmanagedType.LPStr)] public string name;
        public int aroundBodyIndex;
        public double altitudeMetres;
        public double eccentricity;
        public double inclinationRad;
        public double raanRad;
        public double argPeriapsisRad;
        public double meanAnomalyRad;
        public double dryMassKg;
        public double propellantKg;
        public double maxThrustNewtons;
        public double exhaustVelocityMps;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct OmmaSurfaceLaunchRequest
    {
        [MarshalAs(UnmanagedType.LPStr)] public string name;
        public int bodyIndex;
        public double latitudeRad;
        public double longitudeRad;
        public double targetAltitudeMetres;
        public double azimuthRad;
        public double dryMassKg;
        public double propellantKg;
        public double maxThrustNewtons;
        public double exhaustVelocityMps;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct OmmaTransferPlan
    {
        public int valid;
        public double waitSeconds;
        public double transferSeconds;
        public double phaseAngleDeg;
        public double currentPhaseAngleDeg;
        public double vInfinityMps;
        public double departureDeltaVMps;
    }

    public enum AscentPhase
    {
        None = 0,
        Vertical = 1,
        PitchOver = 2,
        Coast = 3,
        Circularize = 4,
        Done = 5,
    }

    public enum BurnFrame
    {
        Prograde = 0,
        Retrograde = 1,
        Normal = 2,
        AntiNormal = 3,
    }

    internal static class OmmaNative
    {
        private const string Dll = "omma";

        [DllImport(Dll)] internal static extern int omma_abi_version();

        [DllImport(Dll)] internal static extern IntPtr omma_create(
            [MarshalAs(UnmanagedType.LPStr)] string scenario, double startDays);
        [DllImport(Dll)] internal static extern void omma_destroy(IntPtr sim);

        [DllImport(Dll)] internal static extern long omma_advance(
            IntPtr sim, double frameDtSeconds, double warp);
        [DllImport(Dll)] internal static extern void omma_step(IntPtr sim, long n);
        [DllImport(Dll)] internal static extern int omma_falling_behind(IntPtr sim);
        [DllImport(Dll)] internal static extern double omma_now_seconds_since_j2000(IntPtr sim);

        [DllImport(Dll)] internal static extern int omma_body_count(IntPtr sim);
        [DllImport(Dll)] internal static extern int omma_body_state(
            IntPtr sim, int index, out OmmaBodyState state);
        [DllImport(Dll)] internal static extern int omma_body_name(
            IntPtr sim, int index, StringBuilder buf, int cap);

        [DllImport(Dll)] internal static extern int omma_craft_count(IntPtr sim);
        [DllImport(Dll)] internal static extern int omma_craft_state(
            IntPtr sim, int index, out OmmaCraftState state);
        [DllImport(Dll)] internal static extern int omma_craft_name(
            IntPtr sim, int index, StringBuilder buf, int cap);
        [DllImport(Dll)] internal static extern int omma_craft_elements(
            IntPtr sim, int index, out OmmaElements elements);

        [DllImport(Dll)] internal static extern long omma_launch(
            IntPtr sim, ref OmmaLaunchRequest request);
        [DllImport(Dll)] internal static extern long omma_launch_ascent(
            IntPtr sim, ref OmmaSurfaceLaunchRequest request);
        [DllImport(Dll)] internal static extern int omma_craft_ascent_phase(
            IntPtr sim, int index);
        [DllImport(Dll)] internal static extern int omma_plan_transfer(
            IntPtr sim, int fromBody, int toBody, double parkingRadiusMetres,
            out OmmaTransferPlan plan);
        [DllImport(Dll)] internal static extern long omma_craft_id(IntPtr sim, int index);
        [DllImport(Dll)] internal static extern int omma_command_delta_v(
            IntPtr sim, long craftId, int frame, double deltaVMps);
        [DllImport(Dll)] internal static extern void omma_cancel_burn(IntPtr sim, long craftId);

        [DllImport(Dll)] internal static extern int omma_body_orbit(
            IntPtr sim, int index, [Out] double[] xyz, int count);
        [DllImport(Dll)] internal static extern int omma_craft_orbit(
            IntPtr sim, int index, [Out] double[] xyz, int count);
    }
}
