using System;
using KRPC.Service;
using KRPC.Service.Attributes;

namespace KSPSerialIO.Hmi
{
    /// <summary>
    /// kRPC service for the Raspberry Pi HMI, available in the VAB and SPH editors (GameScene.Editor; kRPC answers
    /// "Procedure not available in game scene" elsewhere). The Pi polls Revision() about twice a second and calls
    /// ShipReport() only when it changes. Both only read ShipReportCache, filled on Unity's main thread by
    /// EditorShipReport, so an RPC never touches KSP objects. No explicit service Id: kRPC assigns one, and a fixed
    /// one could clash with another mod's service.
    /// </summary>
    [KRPCService(Name = "KSPHMI", GameScene = GameScene.Editor)]
    public static class HmiService
    {
        /// <summary>Increases whenever the ship report changes (and when the editor is left). Poll it (cheap) and
        /// call ShipReport only when it differs from the last revision fetched.</summary>
        [KRPCProcedure]
        public static uint Revision()
        {
            return ShipReportCache.Revision;
        }

        /// <summary>The editor ship report, text format version 1 (docs/hmi.rst "VAB ship report"): one key=value
        /// group per field, header, vehicle summary, one st= line per stage and a kasl= line, or dv=pending.
        /// "" before the first report of an editor session.</summary>
        [KRPCProcedure]
        public static string ShipReport()
        {
            return ShipReportCache.Report;
        }

        /// <summary>A board button pressed on the HMI keyboard: "SAS" (T) or "RCS" (R). It goes through the same
        /// EditorNavigation as a board press, so in the VAB SAS steps to the next editor screen and RCS to the
        /// previous one (the SPH ignores it, as it does board presses). kRPC runs procedures on Unity's main
        /// thread, and the step is only queued here: EditorShipReport applies it in Update.</summary>
        [KRPCProcedure]
        public static void Press(string button)
        {
            if (button == "SAS") EditorNavigation.Press(7);
            else if (button == "RCS") EditorNavigation.Press(6);
            else throw new ArgumentException("button must be SAS or RCS, not '" + button + "'");
        }
    }
}
