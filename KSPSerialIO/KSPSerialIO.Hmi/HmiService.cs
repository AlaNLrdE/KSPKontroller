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
        /// <summary>Increases whenever the ship report changes (and when the editor is left).</summary>
        [KRPCProcedure]
        public static uint Revision()
        {
            return ShipReportCache.Revision;
        }

        /// <summary>The ship report, text format version 1 (see ShipReportFormat); "" before the first report.</summary>
        [KRPCProcedure]
        public static string ShipReport()
        {
            return ShipReportCache.Report;
        }
    }
}
