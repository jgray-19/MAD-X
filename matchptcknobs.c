#include "matchptcknobs.h"
/*______________________________________________________________
matchptcknobs.c
Piotr Skowronski (CERN) 2006
MAD-X module responsible for matching with help of PTC knobs (parameters)

Feb.2007: added proper support for variable limits

It implements MAD-X the command:
match, useptcknobs=true;
(...)
endmatch;

It is basically a macro that performs automatically a set of commands
depicted above as (...) within a rather complicated loop (points 3-17 below). 
It enables fast matching of any order parameters with PTC.

The algorithm:

1. Buffer the key commands (ptc_varyknob, constraint, ptc_setswitch, ptc_twiss or ptc_normal, etc) 
appearing between 
match, useptcknobs=true;
and any of matching actions calls (migrad,lmdif,jacobian, etc)

2.  When matching action appears, 
     a) set "The Current Variables Values" (TCVV) to zero
     b) perform THE LOOP, i.e. points 3-17

3.  Prepare PTC environment (ptc_createuniverse, ptc_createlayout)

4.  Set the user defined knobs (with ptc_knob).

5.  Set TCVV using ptc_setfieldcomp command

6.  Run a PTC command (twiss or normal)

7.  Run a runtime created script that performs a standard matching
    All the user defined knobs are variables of this matching.
   
8.  Evaluate constraints expressions to get the matching function vector (I)

9.  Add the matched values to TCVV

10. End PTC session (run ptc_end)

11. If the matched values are are not close enough to zeroes then goto 3

12. Prepare PTC environment (ptc_createuniverse, ptc_createlayout)

13. Set TCVV using ptc_setfieldcomp command 
  ( --- please note that knobs are not set in this case  -- )
14. Run a PTC command (twiss or normal) 

15. Evaluate constraints expressions to get the matching function vector (II)

16. Evaluate a penalty function that compares matching function vectors (I) and (II)
    See points 7 and 14

17  If the matching function vectors are not similar to each other 
    within requested precision then goto 3

18. Print TCVV, which are the matched values.

______________________________________________________________*/
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <math.h>

#define MAX_CONTRAINS  100
#define MAX_KNOBS  100
#define COMM_LENGTH  500


struct madx_mpk_variable
 {
   char*  name; /*variable name, if initial parameter just copy with mpk_ prefix, if field comp then {elname}_{"kn"/"ks"}{number}*/
   char*  namecv; 
   float  upper;
   float  lower;
   float  trustrange;
   float  step;
   int    knobidx; /*index of the knob that makes paramter of this variable */
   double currentvalue;
   int    kn; /*if a field component then different from -1*/
   int    ks;
   char   IsIniCond; /**/
 };

struct madx_mpk_knob
 {
   char* elname;  /*if a field property element name */
   char* initial; /*if initial condition, name of a variable */
   int*  KN; /*array with filed components*/
   int   nKN;
   int*  KS;
   int   nKS;
   int   exactnamematch;
 };

/************************************/
/*          DATA structure          */
/************************************/
/*constraints*/
int                        madx_mpk_Nconstraints;
char*                      madx_mpk_constraints[MAX_CONTRAINS];

/*knobs*/
int                        madx_mpk_Nknobs;
struct madx_mpk_knob       madx_mpk_knobs[MAX_KNOBS];
char*                      madx_mpk_setknobs[MAX_KNOBS]; /*this is ptc_setknobs*/
struct in_cmd*             madx_mpk_varyknob_cmds[MAX_KNOBS]; /*this is ptc_setknobs*/


/*variables*/
int                        madx_mpk_Nvariables;  /*total number if matching variables >= number of knob commands (one knob command may define more then one knob/variable)*/
struct madx_mpk_variable   madx_mpk_variables[MAX_KNOBS]; 

struct in_cmd* madx_mpk_comm_createuniverse;
struct in_cmd* madx_mpk_comm_createlayout;
struct in_cmd* madx_mpk_comm_setswitch;
struct in_cmd* madx_mpk_comm_calculate;/*ptc_twiss or ptc_normal*/

char twisscommand[]="ptc_twiss, table=ptc_twiss, icase=6, no=2, betx=10, alfx=.0,  bety=10., alfy=0, betz=10., alfz=0;";

void makestdmatchfile(char* fname, char* matchactioncommand);
int  run_ptccalculation(int setknobs,char* readstartval);
void madx_mpk_addfieldcomp(struct madx_mpk_knob* knob, int kn, int ks);
int  madx_mpk_scalelimits(struct madx_mpk_variable* v);
int  findsetknob(char* ename, int exactnamematch,char* initialpar);
int  readstartvalues();
/*******************************************/
/*MAD-X Global Variables used in this file */
/*******************************************/

extern int              debuglevel;
extern int              match_is_on;
extern struct in_cmd*   this_cmd;
extern struct command*  current_command;
extern char             match2_keepexpressions;
extern int              total_const;
extern struct sequence* current_sequ;  /* pointer to currently used sequence */
extern struct el_list*  element_list;

/************************************/
/*MAD-X Functions used in this file */
/************************************/

extern void             pro_input(char* statement);
extern void             process();
extern struct command*  clone_command(struct command*);
extern double           command_par_value(char*, struct command*);
extern char*            command_par_string(char*, struct command*);
extern void             comm_para_(char*, int*, int*, int*, int*, double*, char*, int*);
extern double           get_variable_(char*);
extern void             set_variable_(char*, double*);
extern void*            mymalloc(char* caller, size_t size);
extern void             myfree(char* rout_name, void* p);
extern void             warning(char* t1, char* fmt, ...); 
extern void             warningnew(char* t1, char* fmt, ...); 
extern void             error(char* t1, char* fmt, ...); 
extern int              match2_evaluate_exressions(int i, int k, double* fun_vec);
extern int              name_list_pos(char*, struct name_list*);
extern void             pro_ptc_knob(struct in_cmd* cmd);
extern void             set_command_par_value(char*, struct command*, double);

extern struct element*  find_element(char*, struct el_list*);
extern int              element_vector(struct element*, char*, double*);
extern double           el_par_value(char*, struct element*);
extern void             w_ptc_getnfieldcomp_(int* fibreidx, int* ncomp, double*);
extern void             w_ptc_getsfieldcomp_(int* fibreidx, int* ncomp, double*);

extern int    errorflag;
extern char  *errormessage;

/*_________________________________________________________________________*/
/*_________________________________________________________________________*/
/*_________________________________________________________________________*/
/*******************************/
/* Implementations starts here */
/*******************************/

void madx_mpk_run(struct in_cmd* cmd)
{
/*the main routine of the module, called after at matching action (migrad, lmdif. etc...) */
  char rout_name[] = "madx_mpk_run";
  int i;
  double  tolerance;
  int     calls = 0, maxcalls;
  double  ktar, penalty, penalty2, oldtar = 0.0, tar;
  double  var;
/*  double* matchedvalues;*/
  double* function_vector1 = 0x0;
  double* function_vector2 = 0x0;
  char    ptcend[20];
  char    callmatchfile[200];
  char    matchfilename[300];
  char    matchactioncommand[400];
  char    maxNCallsExceeded = 0;
  char    knobsatmatchedpoint = 0; /*flag indicating that matched values of knobs are with precision close to zero (expansion if valid only close to )*/
  char    matched2 = 0;
  char    matched = 0;
  char    readstartval = 1;
  int     retval;
  FILE*   fdbg = fopen("currentvalues.txt","w");

  matchfilename[0] = 0; /*sign for makestdmatchfile to generate a new file name*/
  
  tolerance = command_par_value("tolerance",cmd->clone);
  if (tolerance < 0)
   {
     warningnew("matchknobs.c: madx_mpk_run","Tolerance is less then 0. Command ignored.");
     return;
   } 
  maxcalls = (int)command_par_value("calls",cmd->clone);
  
  if(madx_mpk_comm_createuniverse == 0x0)
   {
     warningnew("matchknobs.c: madx_mpk_run","ptc_createuniverse command is missing.");
     return;
   }

  if(madx_mpk_comm_createlayout == 0x0)
   {
     warningnew("matchknobs.c: madx_mpk_run","ptc_createlayout is missing.");
     return;
   }

  if(madx_mpk_comm_calculate == 0x0)
   {
     warningnew("matchknobs.c: madx_mpk_run","Neither ptc_twiss nor ptc_normal seen since \"match, use_ptcknob;\"");
     return;
   }

  if (madx_mpk_Nknobs == 0)
   {
     warningnew("matchknobs.c: madx_mpk_run","no knobs seen yet.");
     return;
   }
  
  if (madx_mpk_Nvariables == 0)
   {
     warningnew("matchknobs.c: madx_mpk_run","no variables seen yet.");
     return;
   }
  
  
  sprintf(matchactioncommand,"%s,",cmd->tok_list->p[0]); /*there is no comma after the command name*/
  i = 1;
  while(cmd->tok_list->p[i] != 0x0)
   {
     sprintf(&(matchactioncommand[strlen(matchactioncommand)])," %s",cmd->tok_list->p[i]);
     printf("%s",cmd->tok_list->p[i]);
     i++;
   }
  
  sprintf(&(matchactioncommand[strlen(matchactioncommand)]),";");
  printf("\n");
    
  
  retval = madx_mpk_init();
  
  
  
  strcpy(ptcend,"ptc_end;");
  /*check if ptc starting commands are fine */
  match_is_on = kMatch_NoMatch;   

  match2_keepexpressions = 1;
  
  
  do
   {
     calls++;

     maxNCallsExceeded =  ( (maxcalls > 0)&&(calls > maxcalls) );
     
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("\n\n\n Call %d\n",calls);
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     printf("###########################################################################################\n"); 
     
     printf("\n CURRENT VALUES \n");
     for (i=0;i<madx_mpk_Nvariables;i++)
      {
        printf("%16s   ",madx_mpk_variables[i].name);
      }
     printf("\n"); 
     for (i=0;i<madx_mpk_Nvariables;i++)
      {
        printf("%16f   ",madx_mpk_variables[i].currentvalue);
      }
     
    
     printf("\n\nCalling TWISS with KNOBS\n");
     run_ptccalculation(1,&readstartval);
     
     if (errorflag)
      {
        error("Matching With Knobs","PTC calculation ended with an error. Check your setting and matching limits.");
        pro_input(ptcend);
        goto cleaning;
      }
     
     /*set all variables to 0*/
     var = 0.0;
     for (i=0;i<madx_mpk_Nvariables;i++)
      {
        set_variable_(madx_mpk_variables[i].name, &var);
        set_variable_(madx_mpk_variables[i].namecv, &(madx_mpk_variables[i].currentvalue));
      }
     
     printf("\n\nDoing Match on KNOBS\n");
     makestdmatchfile(matchfilename,matchactioncommand);
     printf("matchfilename is %s\n",matchfilename);
  
     sprintf(callmatchfile,"call, file=\"%s\" ;",matchfilename);
     pro_input(callmatchfile);
     
     if (function_vector1 == 0x0)
      { 
        printf("total_const is %d\n",total_const);
        function_vector1 = (double*)mymalloc("madx_mpk_run",(total_const+1)*sizeof(double));
        function_vector2 = (double*)mymalloc("madx_mpk_run",(total_const+1)*sizeof(double));
      } 
     
     printf("\n\nPeeking Function_Vector 1\n");
     i = match2_evaluate_exressions(0,0,function_vector1);
     printf("match2_evaluate_exressions returned fun_vector of length %d\n",i);

     
     pro_input(ptcend);

     printf("\n\n\n");
     printf("\nFunction_Vector 1:\n");
   
     ktar = 0.0;
     fprintf(fdbg,"%d",calls);
     for (i=0;i<madx_mpk_Nvariables;i++)
      {
        var = get_variable_(madx_mpk_variables[i].name);
        madx_mpk_variables[i].currentvalue += var;
        printf("Matched knob %s = %f, new current val %f  \n",
                       madx_mpk_variables[i].name, var, 
                       madx_mpk_variables[i].currentvalue);
        fprintf(fdbg," %f ", madx_mpk_variables[i].currentvalue);
        ktar += var*var;
      }
     printf("\n");
     tar = get_variable_("tar");
     fprintf(fdbg," KTAR=%E TAR=%E\n",ktar, tar);
     fflush(0x0);
     printf("KTAR=%E \n", ktar ); 
     
     if ( (oldtar > 0) && (tar > oldtar))
       {
        fprintf(fdbg,"Narrowing trust ranges\n"); 
        for (i=0;i<madx_mpk_Nvariables;i++)
         {
           madx_mpk_variables[i].trustrange /= 2.0;
         }
       }

     oldtar = tar;
     
     if (ktar < tolerance)
      { 
        knobsatmatchedpoint = 1;
        printf("Matched values of knobs are close to zeroes within the tolerance\n");
      }  
     else
      {
        knobsatmatchedpoint = 0;
        printf("Matched values of knobs are NOT close to zeroes within the tolerance\n");
        if (!maxNCallsExceeded) continue;
      }

     
     printf("\n\nCalling TWISS with NO knobs\n");
     run_ptccalculation(0,&readstartval);
     
     printf("\n\nPeeking Function_Vector 2\n");
     i = match2_evaluate_exressions(0,0,function_vector2);

     pro_input(ptcend);
    
     
     
     penalty  = 0.0;
     penalty2 = 0.0;

     printf("\n Function_Vector 2:\n");
     
     for (i=0; i< total_const; i++)
      {
        penalty += function_vector2[i]*function_vector2[i];
        var = function_vector2[i] - function_vector1[i];
        if (debuglevel>1) printf("Constraint%d: %f diff=%f \n",i,function_vector2[i],var);
        penalty2 += var*var;
      }
     
     fprintf(fdbg,"Verification: penalty=%E  penalty2=%E \n",penalty, penalty2);

     printf("Closenes of the function vectors with and without knobs %e\n",penalty2);
     
     if (penalty2 < tolerance)
      { 
        matched2 = 1;
        printf("Constraints values with and without knobs within the tolerance\n");
      }  
     else
      {
        matched2 = 0;
        printf("Constraints values with and without knobs NOT within the tolerance\n");
      }
    
     matched = matched2 && knobsatmatchedpoint;
   
     
     printf("matched=%d, maxNCallsExceeded=%d\n",matched,maxNCallsExceeded);
     
   }
  while( !(matched || maxNCallsExceeded) );
  
  match2_keepexpressions = 0;
  match_is_on = kMatch_PTCknobs;   
  
  fflush(0);
  printf("\n\n");
  printf("========================================================================== \n");
  printf("== Matching finished successfully after %3.3d calls                       == \n",calls);
  
  printf("== Precision of Taylor expansion the result point = %E        ==\n",ktar);
  printf("==----------------------------------------------------------------------== \n");
  printf("==      Variable                   Final Value                          == \n");
  printf("\n");
  
  
  for (i=0;i<madx_mpk_Nvariables;i++)
   {
     set_variable_(madx_mpk_variables[i].name,&(madx_mpk_variables[i].currentvalue));
     printf("== %d %16s          %+f                                  == \n",
             i,madx_mpk_variables[i].name, madx_mpk_variables[i].currentvalue);
     printf("\n");           
   }
  
  printf("==========================================================================\n");
  
  cleaning:
  myfree(rout_name,function_vector1);
  myfree(rout_name,function_vector2);
  
  function_vector1 = 0x0;
  function_vector2 = 0x0;
  
  fclose(fdbg);
/*  remove(matchfilename);    */
}
/*_________________________________________________________________________*/

void madx_mpk_prepare()
{
/* prepares the internal variables*/
  int i; /**/
  
  printf("I am in MATCH PTC WITH KNOBS\n");

  madx_mpk_Nconstraints = 0;
  madx_mpk_Nknobs = 0;
  madx_mpk_Nvariables = 0;
  
  madx_mpk_comm_createuniverse = 0x0;
  madx_mpk_comm_createlayout = 0x0;
  madx_mpk_comm_setswitch = 0x0;
  madx_mpk_comm_calculate = 0x0;
  
  for (i = 0; i< MAX_KNOBS; i++)  
   {

     madx_mpk_knobs[i].elname  = 0x0;
     madx_mpk_knobs[i].initial = 0x0;
     madx_mpk_knobs[i].KN      = 0x0;
     madx_mpk_knobs[i].nKN     = 0;
     madx_mpk_knobs[i].KS      = 0x0;
     madx_mpk_knobs[i].nKS     = 0;  
     madx_mpk_knobs[i].exactnamematch = 1;

     madx_mpk_variables[i].name          = 0x0;
     madx_mpk_variables[i].upper         = 0.0;
     madx_mpk_variables[i].lower         = 0.0;
     madx_mpk_variables[i].trustrange    = 0.0;
     madx_mpk_variables[i].step          = 0.0;
     madx_mpk_variables[i].knobidx       = -1;
     madx_mpk_variables[i].currentvalue  = 0.0;
     
     madx_mpk_variables[i].kn            = -1;
     madx_mpk_variables[i].ks            = -1;
     madx_mpk_variables[i].IsIniCond     = -1;
     
     
   }
  return;
  
}
/*_________________________________________________________________________*/

int madx_mpk_init()
{
  /*performs initialization, 
    prepares commands for exacution out of gethered user information*/

  /*Create ptc_command: just copy paste the valid parameters - no error checking*/

  char                           vary[COMM_LENGTH];
  int i,k;
  
  for(i=0; i< madx_mpk_Nknobs; i++)
   {
     sprintf(vary,"ptc_knob, ");
     
     if (madx_mpk_knobs[i].elname)
      {
        sprintf(&(vary[strlen(vary)]),"element=%s, ",madx_mpk_knobs[i].elname);

        if (madx_mpk_knobs[i].nKN > 0)
         {
         
           sprintf(&(vary[strlen(vary)]),"kn=");
            
           for (k=0; k < madx_mpk_knobs[i].nKN; k++)
            {
              sprintf(&(vary[strlen(vary)]),"%d,",madx_mpk_knobs[i].KN[k]);
            }  
         }

        if (madx_mpk_knobs[i].nKS > 0)
         {
         
           sprintf(&(vary[strlen(vary)]),"ks=");
            
           for (k=0; k < madx_mpk_knobs[i].nKS; k++)
            {
              sprintf(&(vary[strlen(vary)]),"%d,",madx_mpk_knobs[i].KS[k]);
            }  
         }

        /*this is the last one anyway if ename is specified ; at the end*/
        if (madx_mpk_knobs[i].exactnamematch)
         {
          sprintf(&(vary[strlen(vary)]),"exactmatch=%s; ","true");
         }
        else
         { 
          sprintf(&(vary[strlen(vary)]),"exactmatch=%s; ","false");
         } 

      } 
     
     if (madx_mpk_knobs[i].initial)
      {
        sprintf(&(vary[strlen(vary)]),"initial=%s; ",madx_mpk_knobs[i].initial);
      }  

     madx_mpk_setknobs[i] = (char*)mymalloc("madx_mpk_addvariable",strlen(vary)*sizeof(char));
     strcpy(madx_mpk_setknobs[i],vary);

     printf("madx_mpk_setknobs[%d]= %s\n",i,madx_mpk_setknobs[i]);

     
   }
    
  return 0;
}
/*_________________________________________________________________________*/

void madx_mpk_addconstraint(const char* constr)
{
  char* buff;
  int l;
  if (constr == 0x0) return;
  l = strlen(constr);
  if (l<1) return;
  l++;
  buff = (char*)mymalloc("madx_mpk_addconstraint",l*sizeof(char));
  strcpy(buff,constr);
  printf("madx_mpk_addconstraint: got %s\n",buff);
  madx_mpk_constraints[madx_mpk_Nconstraints++] = buff;
  
}
/*_________________________________________________________________________*/

void madx_mpk_addvariable(struct in_cmd* cmd)
{
  struct command_parameter_list* c_parameters;
  struct name_list*              c_parnames;
  char                           vary[COMM_LENGTH];
  int                            slen;
  char*                          string;
  char*                          ename;
  char*                          initialpar;
  int                            exactnamematch;/*0 for families with name starting with ename, 1 for for element having name ename*/
  int                            kn,ks;
/*  int                            int_arr[100];*/
/*  int                            i;*/
  int                            knobidx; /*index of setknob command for this varyknob*/
  
  
  struct madx_mpk_variable*      v = 0x0;
  
  if (cmd == 0x0) return;

  c_parameters = cmd->clone->par;
  c_parnames   = cmd->clone->par_names;

  /*get a name for the variable*/
  ename  = command_par_string("element",cmd->clone);
  initialpar = command_par_string("initial",cmd->clone);
  if (( ename == 0x0 ) && ( initialpar == 0x0 ))
  {
    error("matchknobs.c: madx_mpk_addvariable",
          "Neither element nor initial parameter specified. Command ignored!");
    return;
  }

  if ( ename && initialpar)
   {
     error("matchknobs.c: madx_mpk_addvariable",
           "Single command may define only one of two, field component or initial parameter. Command ignored!");
     return;
   }
  
  kn =  (int)command_par_value("kn",cmd->clone);
  ks =  (int)command_par_value("ks",cmd->clone);

  if ( ename && (kn >= 0) && (ks >=0) )
   {
     error("matchknobs.c: madx_mpk_addvariable",
           "Single command may define only one field component, not ks and kn together. Command ignored.");
     return;
   }
  
  exactnamematch = command_par_value("exactmatch",cmd->clone);
  
  knobidx = findsetknob(ename,exactnamematch,initialpar);
  
  if (knobidx < 0)
   {
     error("madx_mpk_addvariable","Error occured while adding this variable.");
     return;
   }
  
  /*so we make a new variable...*/

  v = &madx_mpk_variables[madx_mpk_Nvariables];
  
  v->upper = command_par_value("upper",cmd->clone);
  v->lower = command_par_value("lower",cmd->clone);
  v->trustrange = command_par_value("trustrange",cmd->clone);
  v->step = command_par_value("step",cmd->clone);
  v->knobidx = knobidx;

  if (initialpar)
   {
     madx_mpk_knobs[madx_mpk_Nknobs].initial = (char*)mymalloc("madx_mpk_addvariable",strlen(initialpar)*sizeof(char));
     strcpy(madx_mpk_knobs[knobidx].initial, initialpar);
     
     sprintf(vary,"%mpk_%s",initialpar);
     v->name = (char*)mymalloc("madx_mpk_addvariable",strlen(vary)*sizeof(char));  
     strcpy(v->name,vary);

     sprintf(vary,"%mpk_%s_0",initialpar);
     v->namecv = (char*)mymalloc("madx_mpk_addvariable",strlen(vary)*sizeof(char));  
     strcpy(v->namecv,vary);
     
     v->IsIniCond = 1;
     v->kn = -1;
     v->ks = -1;
   }
  


  if (ename)
   {
     madx_mpk_knobs[knobidx].elname = (char*)mymalloc("madx_mpk_addvariable",strlen(ename)*sizeof(char));
     strcpy(madx_mpk_knobs[knobidx].elname, ename);
  

     if (exactnamematch != 0)
      {
       madx_mpk_knobs[knobidx].exactnamematch = 1;
      }
     else
      {
       madx_mpk_knobs[knobidx].exactnamematch = 0;
      }

     if (kn >=0)
      {
        /*create variable name, use vary as a temp buffer */
        sprintf(vary,"%s_kn%d",ename,kn);
        madx_mpk_addfieldcomp(&(madx_mpk_knobs[knobidx]), kn, -1);
      } 

     if (ks >=0)
      {
        /*create variable name, use vary as a temp buffer */
        sprintf(vary,"%s_ks%d",ename,kn);
        madx_mpk_addfieldcomp(&(madx_mpk_knobs[knobidx]), -1, ks);
      } 

     
     slen = strlen(vary);
     string = (char*)mymalloc("madx_mpk_addvariable",slen*sizeof(char));  
     strcpy(string,vary);
     v->name = string;

     vary[slen  ] = '_';
     vary[slen+1] = '0';
     vary[slen+2] =  0;/*end of string*/
     slen  = slen+2;
     
     string = (char*)mymalloc("madx_mpk_addvariable",slen*sizeof(char));  
     strcpy(string,vary);
     v->namecv = string;

     v->kn = kn;
     v->ks = ks;
     v->IsIniCond = 0;
     
     madx_mpk_scalelimits(v);
    
   }

  madx_mpk_Nvariables++;
  
  
  printf("Added new knobs: now there is\n  knobs: %d\n  variables: %d\n",
          madx_mpk_Nknobs,madx_mpk_Nvariables);
}
/*_________________________________________________________________________*/

int findsetknob(char* ename, int exactnamematch, char* initialpar)
{
  /*
    finds a knob that corresponds to this name, and detects eventual errors
    if not found returns fresh empty knob
  */
  int i;
  int cmpres;
  int result = madx_mpk_Nknobs; /*a new empty one*/
  char* bconta;
  char* acontb;

  if (ename)
   {
     for (i = 0; i < madx_mpk_Nknobs; i++)
      {
        
        cmpres = strcmp(ename,madx_mpk_knobs[i].elname);
        if (cmpres == 0)
         {
           if ( exactnamematch == madx_mpk_knobs[i].exactnamematch )
            {
              result = i; /*the same element or family*/
              continue; /*to find out if there is any setting problem*/
            }
           else
            {
              error("findsetknob","A knob for such named element(s) found, but name matching flag does not agree.");
              return -i;
            } 
         }
         
        /*
         if a not-exact and b exact,  b can not contain a 
         if a not-exact and b non-exact,  b can not contain a and a can not contain b
         if a exact, and b not-exact, a can not contain b
        */
        acontb = strstr(ename, madx_mpk_knobs[i].elname);
        bconta = strstr(madx_mpk_knobs[i].elname, ename);
        if ( (acontb && (exactnamematch == 0)) || (bconta && (madx_mpk_knobs[i].exactnamematch  == 0)) )
         {
           error("findsetknob",
                 "This variable (name %s, exactmatch %d) can cause ambiguity with another already defined variable (name %s, exactmatch %d)",
                  ename, exactnamematch, madx_mpk_knobs[i].elname, madx_mpk_knobs[i].exactnamematch);
           return -i;
         }
      } 
   }
      
  if (initialpar)
   {
     for (i = 0; i < madx_mpk_Nknobs; i++)
      {
        if (madx_mpk_knobs[i].initial)
         {
           if (( strcmp(initialpar,madx_mpk_knobs[i].initial) == 0 ))
            {
            
              error("findsetknob","Such initial parameter is already defined");
              return -i;
            }
         }
      }
   }
  
  if (result == madx_mpk_Nknobs) 
   {
     printf("findsetknob: returning fresh setknob for %s.\n", ename);
     madx_mpk_Nknobs++;  /*we have not found a setknob to add this vary knob*/
   }
  else
   {
     printf("findsetknob: Found an old setknob for %s.\n", ename);
   } 

  return   result;
 
}

/*_________________________________________________________________________*/

void madx_mpk_setcreateuniverse(struct in_cmd* cmd)
{
  cmd->clone_flag = 1; /* do not delete for the moment*/
  cmd->label =  (char*)mymalloc("madx_mpk_setcreateuniverse",24*sizeof(char));
  strcpy(cmd->label,"matchptcknob_ptc_CU");

  madx_mpk_comm_createuniverse = cmd;
}
/*_________________________________________________________________________*/
void madx_mpk_setcreatelayout(struct in_cmd* cmd)
{
  cmd->clone_flag = 1; /* do not delete for the moment*/
  cmd->label =  (char*)mymalloc("madx_mpk_setcreatelayout",24*sizeof(char));
  strcpy(cmd->label,"matchptcknob_ptc_CL");
  madx_mpk_comm_createlayout = cmd;
}
/*_________________________________________________________________________*/
void madx_mpk_setsetswitch(struct in_cmd* cmd)
{
  cmd->clone_flag = 1; /* do not delete for the moment*/
  cmd->label =  (char*)mymalloc("madx_mpk_setsetswitch",24*sizeof(char));
  strcpy(cmd->label,"matchptcknob_ptc_SSW");
  madx_mpk_comm_setswitch = cmd;
}
/*_________________________________________________________________________*/

void madx_mpk_setcalc(struct in_cmd* cmd)
{
  cmd->clone_flag = 1; /* do not delete for the moment*/
  cmd->label =  (char*)mymalloc("madx_mpk_setcalc",24*sizeof(char));
  strcpy(cmd->label,"matchptcknob_ptc_CMD");
  madx_mpk_comm_calculate = cmd;
}
/*_________________________________________________________________________*/

int  readstartvalues()
{
/*
   reads the starting values of variable to initializ currentvalue
*/
  
  int i;
  int n;
  int ncomp;
  double nvalue;
  struct node*                node = 0x0;
  struct madx_mpk_variable*      v = 0x0;
  struct madx_mpk_knob*         kn = 0x0;
  char  buff[50];
  char* p;
  
  for (i = 0; i < madx_mpk_Nvariables; i++)
   {
     v = &(madx_mpk_variables[i]);
     kn = &(madx_mpk_knobs[v->knobidx]);
     
     if (v->IsIniCond)
      {
        v->currentvalue = command_par_value(kn->initial, madx_mpk_comm_calculate->clone);
        printf("Initialized current value for %s to %f\n", 
                kn->initial,v->currentvalue);
      }
     else
      {
        printf("Achtung! Ignoring families for the time being\n");
        n =  0;
        node = current_sequ->range_start;
        
        while (node != 0x0)
         {
           strcpy(buff,node->name);
           p = strstr(buff,":");
           if (p) *p = 0;
           
           printf("Comparing %s %s\n",node->name,kn->elname); 
           if ( strcmp(buff,kn->elname) == 0 )
            {
              break;
            }

           n++;
           
           node = node->next;

           if ( node == current_sequ->range_start )
            { 
              fatal_error("readstartvalues: Can not find element: ",kn->elname);
              return 1; 
            }
         }
        
        if (v->kn >=0)
         {
           ncomp = v->kn;
           w_ptc_getnfieldcomp_(&n,&ncomp, &nvalue);
           v->currentvalue = nvalue;
         }
        else
         {
           ncomp = v->ks;
           w_ptc_getsfieldcomp_(&n,&ncomp, &nvalue);
           v->currentvalue = nvalue;
         }   

        printf("Got %f for %s\n",v->currentvalue,kn->elname);

      } 
   }
  
  
  return 0;
}

int madx_mpk_scalelimits(struct madx_mpk_variable* v)
{

  double nvect[FIELD_MAX];
  double svect[FIELD_MAX];
  
  struct element* el = 0x0;
  double bn=0,bs=0, tmpf;
  double l;
  int    n;
  int    fieldorder = 1 + (v->kn >= 0)?v->kn:v->ks;/*PTC nomenclature, 1 dipol, 2 quad ...*/
  
  
  if ( ( v->kn == 0) || (v->ks == 0) )
   {
     printf("Dipol limits don't need to be scaled\n");
     return 1;
   }
  
  if (v == 0x0)
   {
     error("madx_mpk_scalelimits","Passed pointer to variable is NULL");
     return 1;
   }

  el = find_element(madx_mpk_knobs[v->knobidx].elname, element_list);
  if (el == 0x0)
   {
     error("madx_mpk_scalelimits","Can not find element named %s in the current sequence",madx_mpk_knobs[v->knobidx].elname);
     return 1;
   }
  
  /* n = element_vector(el, "knl", nvect);*/
  /* n = element_vector(el, "ksl", svect);*/
  
  printf("Element %s\n",el->name);
  if (el->parent)
    printf("Parent name %s def_type %d\n", el->parent->name, el->parent->def_type);

  if (el->base_type)
    printf("base_type name %s def_type %d\n", el->base_type->name, el->base_type->def_type);
  
  l = el_par_value("l",el);
  if (l <= 0.0)
   {
     l = 1.0; /*zero lentgh elements has field defined such, compatible with madx_ptc_module.f90:SUMM_MULTIPOLES_AND_ERRORS*/
   }
  v->upper = v->upper/tgamma(fieldorder)/l;
  v->lower = v->lower/tgamma(fieldorder)/l;
        
  printf("Set limits to field order (PTC) %d: %f %f\n", fieldorder, v->lower, v->upper ); 
   
     
  
  return 0;
  
}  

void madx_mpk_addfieldcomp(struct madx_mpk_knob* knob, int kn, int ks)
{
/*
  adds a new field component to a knob
*/
  void* ptr = 0x0;
    
  if (knob == 0x0)
   {
     warning("madx_mpk_addfieldcomp","knob parameter is null");
     return;
   }
  
  if (kn >= 0) 
   { 
     knob->nKN++;
     ptr = (void*)knob->KN;
     knob->KN = (int*)realloc(knob->KN, knob->nKN * sizeof(int));
     knob->KN[knob->nKN - 1] = kn;
     
     if (ptr != knob->KN)
      {
        free(ptr);
      }
   }

  if (ks >= 0) 
   {
     knob->nKS++;
     ptr = (void*)knob->KS;
     knob->KS = (int*)realloc(knob->KS, knob->nKS * sizeof(int));
     knob->KS[knob->nKS - 1] = ks;

     if (ptr != knob->KS)
      {
        free(ptr);
      }
   }
  
}
/*_________________________________________________________________________*/

void madx_mpk_end()
{

/*
  remove_from_command_list(this_cmd->clone->name,stored_commands);


*/


  match_is_on = kMatch_NoMatch;

}


/*_________________________________________________________________________*/
/*_________________________________________________________________________*/
/*_________________________________________________________________________*/

void makestdmatchfile(char* fname, char* matchactioncommand)
{
  FILE* f = 0x0;
  struct madx_mpk_variable*      v = 0x0;
  int  i;
  unsigned int anumber = abs(time(0x0)*rand());
  float lower, upper;
  
  printf("I am in makestdmatchfile, file name is >%s<\n", fname);
  
  while(f == 0x0)
   {/*repeat until generate unique file name*/
     if (fname[0] == 0)
      { /*if string is empty*/
        sprintf(fname,"/tmp/match_ptcknobs_%d.madx",anumber);
      }  
     f = fopen(fname,"w");
     if (f == 0x0)
      {
        warningnew("makestdmatchfile","Could not open file %s",fname);
      }
     else
      {
        printf("makestdmatchfile: Knob Matching file in %s\n",fname); 
      } 
   }

  printf("Std Match file name is %s\n",fname);
  
  fprintf(f,"assign, echo=/tmp/mpk_stdmatch.out;\n");
  
  fprintf(f,"match, use_macro;\n");
  
  for (i = 0; i<madx_mpk_Nvariables; i++)
   {

     v = &madx_mpk_variables[i];
     
     /*create vary command as string*/
     printf("\ncurrentvalue=%f trustrange=%f lower=%f upper=%f\n",
           v->currentvalue, v->trustrange , v->lower ,v->upper);
           
     if ( (v->currentvalue - v->trustrange) < v->lower )
      {
        lower = v->currentvalue - v->lower;
      }
     else
      {
        lower = - v->trustrange;
      }

     if ( (v->currentvalue + v->trustrange) > v->upper )
      {
        upper = v->upper - v->currentvalue;
      }
     else
      {
        upper =  v->trustrange;
      }

     printf("   vary, name=%s, lower= %g, upper = %g;\n",
                      v->name,  lower,     upper);
     
     fprintf(f,"   vary, name=%s, step= %g, lower= %g, upper = %g;\n",
                      v->name,  v->step, lower,     upper);

   }
  fprintf(f,"   \n");

  fprintf(f,"   m1: macro =  \n");
  fprintf(f,"     {\n");

  printf("Std Match file name is %s\n",fname);
  printf("There is %d variables.\n",madx_mpk_Nvariables);
  
  for (i = 0; i<madx_mpk_Nvariables; i++)
   {
     if (madx_mpk_variables[i].IsIniCond )
      {
        fprintf(f,"      ptc_setknobvalue ,element=%s, kn=-1 ,ks=-1, value=%s;\n",
                    	   madx_mpk_knobs[ madx_mpk_variables[i].knobidx ].initial, 
                          	   madx_mpk_variables[i].name);
      }
     else
      { 
        fprintf(f,"      ptc_setknobvalue ,element=%s, kn=%d ,ks=%d, value=%s;\n",
                    	   madx_mpk_knobs[ madx_mpk_variables[i].knobidx ].elname, 
		   madx_mpk_variables[i].kn,
		   madx_mpk_variables[i].ks,
                          	   madx_mpk_variables[i].name);
      }
   /*  if (debuglevel)*/
        fprintf(f,"      value , %s, %s;\n", madx_mpk_variables[i].name, madx_mpk_variables[i].namecv );
   }


  fprintf(f,"      value , table(ptc_twiss,theend,beta11);\n");
  fprintf(f,"      value , table(ptc_twiss,qf1,beta11);\n");
  
  fprintf(f,"     };\n");


  for (i = 0; i<madx_mpk_Nconstraints; i++)
   {
     fprintf(f,"   %s;\n",madx_mpk_constraints[i]);
   }
   

  fprintf(f,"  %s\n",matchactioncommand);

  fprintf(f,"endmatch;\n");

  fprintf(f,"assign, echo=\terminal;\n");
  
  fclose(f);

}
/*_________________________________________________________________________*/

int run_ptccalculation(int setknobs, char* readstartval)
{
  int i;
  char buff[500];
  char* iniparname;
  
  this_cmd = madx_mpk_comm_createuniverse;
  current_command =  madx_mpk_comm_createuniverse->clone;
  process();

  this_cmd = madx_mpk_comm_createlayout;
  current_command =  madx_mpk_comm_createlayout->clone;
  process();

  if (madx_mpk_comm_setswitch)
   {
     this_cmd = madx_mpk_comm_createlayout;
     current_command =  madx_mpk_comm_createlayout->clone;
     process();
   }

  if (*readstartval == 0) 
   { 
     for(i=0;i<madx_mpk_Nvariables;i++)
      {
        if (madx_mpk_variables[i].IsIniCond)
         { /*Set the initial parameter in ptc_twiss*/
            iniparname = madx_mpk_knobs[ madx_mpk_variables[i].knobidx ].initial;
            set_command_par_value( iniparname, madx_mpk_comm_calculate->clone ,madx_mpk_variables[i].currentvalue);
         }
        else
         { 
            sprintf(buff,"ptc_setfieldcomp, element=%s, kn=%d, ks=%d, value=%f;",
                    	    madx_mpk_knobs[ madx_mpk_variables[i].knobidx ].elname, 
		    madx_mpk_variables[i].kn,
		    madx_mpk_variables[i].ks,
                    	    madx_mpk_variables[i].currentvalue);
            /*printf("%s\n",buff);*/
            pro_input(buff);
         }
      }
   }

  if(setknobs)
   {
    
    for(i=0;i<madx_mpk_Nknobs;i++)
     {
/*        printf("Setting knob %d: \n%s\n",i,madx_mpk_setknobs[i]);*/
        pro_input(madx_mpk_setknobs[i]);
     }
   }
  else
   {
     printf("Knob Setting Is not requested this time.\n");
   }
   
/*  pro_input(twisscommand);*/

  printf("Running ptc_twiss or ptc_normal.\n");
  
  this_cmd = madx_mpk_comm_calculate;
  current_command =  madx_mpk_comm_calculate->clone;
  process();


  if ( *readstartval )
   {
     readstartvalues();
     *readstartval = 0;
   }

  
  return 0;
} 
