Skip to content
Your account has been flagged.
Because of that, your profile is hidden from the public. If you believe this is a mistake, contact support to have your account status reviewed.
open source solar pi
Repositories0
Code142K
Commits1
Issues384
Marketplace0
Topics0
Wikis288
Users0
Language

Sort

142,352 code results
@salzheimer
salzheimer/Spring2014_CapstoneProject – Solar_BusBarn_PI_Repository.cs
Showing the top seven matches
Last indexed on Jun 27, 2018
C#
        public Core.Models.Solar_BusBarn GetByTime(Core.PiServerTableTags.SolarSources solarSource, string time)
        {
            var selectCmd = SelectCommand("*", "piinterp", GetEnumDescription(solarSource), time);
            return solar;
        }

        public List<Core.Models.Solar_BusBarn> GetByTime(Core.PiServerTableTags.SolarSources solarSource, string startDateTime, string endDateTime, string timeStep = "1h")
@salzheimer
salzheimer/Spring2014_CapstoneProject – Solar_CarCharger_PI_Repository.cs
Showing the top seven matches
Last indexed on Jun 27, 2018
C#
        public Core.Models.Solar_CarCharger GetToday(Core.PiServerTableTags.SolarSources solarSource)
        {
            var selectCmd = SelectCommand("*", "piinterp", GetEnumDescription(solarSource), "today");
        public Core.Models.Solar_CarCharger GetByTime(Core.PiServerTableTags.SolarSources solarSource, string time)
@salzheimer
salzheimer/Spring2014_CapstoneProject – SolarRadiation_PI_Repository.cs
Showing the top six matches
Last indexed on Jun 27, 2018
C#
    public class SolarRadiation_PI_Repository : PiServerRepositoryBase, Core.Data.PiServer.ISolarRadiationRespository
    {
        

        public Core.Models.SolarRadiation GetToday(SolarRadiationSources source)
@gkulkarni
gkulkarni/HeatTheIGM – main.f90
Showing the top five matches
Last indexed on Jun 29, 2018
Fortran
  JNSLN = JEANS_LENGTH(TEMPHVA,Z) ! Mpc 

  JNSM = (4.0_PREC*PI*RHO_BARYON*JNSLN**3)/3.0_PREC ! 10^10 M_solar
  JMHARR(COUNTR) = JNSM ! 10^10 M_solar 
     source_pop2 = sm_pop2
     source_pop3 = sm_pop3
     sfrarr(countr-1) = source*1.0e10_prec ! M_solar yr^-1 Mpc^-3
@gkulkarni
gkulkarni/ReionEq – main_oldlimmag.f90
Showing the top five matches
Last indexed on Jun 29, 2018
Fortran
  JNSM = (4.0_PREC*PI*RHO_BARYON*JNSLN**3)/3.0_PREC ! 10^10 M_solar
  JMHARR(COUNTR) = JNSM ! 10^10 M_solar 
     source_pop2 = sm_pop2
     source_pop3 = sm_pop3
     sfrarr(countr-1) = source*1.0e10_prec ! M_solar yr^-1 Mpc^-3
@gkulkarni
gkulkarni/ReionEq – main.f90
Showing the top three matches
Last indexed on Jun 29, 2018
Fortran
     source_pop3 = sm_pop3
     sfrarr(countr-1) = source*1.0e10_prec ! M_solar yr^-1 Mpc^-3
     sfrarr_pop2(countr-1) = source_pop2*1.0e10_prec ! M_solar yr^-1 Mpc^-3
     sfrarr_pop3(countr-1) = source_pop3*1.0e10_prec ! M_solar yr^-1 Mpc^-3
@highapect
highapect/Open-Rails-4022 – SunMoonPos.cs
Showing the top six matches
Last indexed on Jul 12, 2018
C#
            // Solar elevation angle, radians. Currently not used.
            //          double solarElevationAngle = MathHelper.PiOver2 - Math.Acos(solarZenithCosine);
            //          if (clockTime > 0.5)
            //              solarAzimuthAngle = MathHelper.TwoPi - solarAzimuthAngle;
@pzgulyas
pzgulyas/OpenRails – SunMoonPos.cs
Showing the top seven matches
Last indexed on Jul 1, 2018
C#
            //          double solarElevationAngle = MathHelper.PiOver2 - Math.Acos(solarZenithCosine);

            // Solar azimuth cosine. This is the Z COORDINATE of the solar Vector.
            //          if (clockTime > 0.5)
            //              solarAzimuthAngle = MathHelper.TwoPi - solarAzimuthAngle;
@gkulkarni
gkulkarni/HeatTheIGM – main_yld.f90
Showing the top five matches
Last indexed on Jun 29, 2018
Fortran
  ! JNSLN = JEANS_LENGTH(TEMPHVA,Z) ! Mpc 

  JNSM = (4.0_PREC*PI*RHO_BARYON*JNSLN**3)/3.0_PREC ! 10^10 M_solar
     sfrarr(countr-1) = source*1.0e10_prec ! M_solar yr^-1 Mpc^-3
     sfrarr_pop2(countr-1) = source_pop2*1.0e10_prec ! M_solar yr^-1 Mpc^-3
@gkulkarni
gkulkarni/ReionEq – main_mysore.f90
Showing the top seven matches
Last indexed on Jun 29, 2018
Fortran
  JNSM = (4.0_PREC*PI*RHO_BARYON*JNSLN**3)/3.0_PREC ! 10^10 M_solar
  JMHARR(COUNTR) = JNSM ! 10^10 M_solar 
     source = sm 
     source_pop2 = sm_pop2
     source_pop3 = sm_pop3
     sfrarr(countr-1) = source*1.0e10_prec ! M_solar yr^-1 Mpc^-3
© 2019 GitHub, Inc.
Terms
Privacy
Security
Status
Help
Contact GitHub
Pricing
API
Training
Blog
About
Press h to open a hovercard with more details.
