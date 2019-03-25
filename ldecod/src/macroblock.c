
/*!
 ***********************************************************************
 * \file macroblock.c
 *
 * \brief
 *     Decode a Macroblock
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *    - Inge Lille-Lang�y               <inge.lille-langoy@telenor.com>
 *    - Rickard Sjoberg                 <rickard.sjoberg@era.ericsson.se>
 *    - Jani Lainema                    <jani.lainema@nokia.com>
 *    - Sebastian Purreiter             <sebastian.purreiter@mch.siemens.de>
 *    - Thomas Wedi                     <wedi@tnt.uni-hannover.de>
 *    - Detlev Marpe                    <marpe@hhi.de>
 *    - Gabi Blaettermann
 *    - Ye-Kui Wang                     <wyk@ieee.org>
 *    - Lowell Winger                   <lwinger@lsil.com>
 ***********************************************************************
*/

#include "contributors.h"

#include <math.h>

#include "global.h"
#include "mbuffer.h"
#include "elements.h"
#include "errorconcealment.h"
#include "macroblock.h"
#include "fmo.h"
#include "cabac.h"
#include "vlc.h"
#include "image.h"
#include "mb_access.h"
#include "biaridecod.h"
#include "block.h"
#include "transform8x8.h"
#include "mc_prediction.h"

#if TRACE
#define TRACE_STRING(s) strncpy(currSE.tracestring, s, TRACESTRING_SIZE)
#else
#define TRACE_STRING(s) // do nothing
#endif

extern int last_dquant;
extern ColocatedParams *Co_located;
extern ColocatedParams *Co_located_JV[MAX_PLANE];  //!< Co_located to be used during 4:4:4 independent mode decoding

/*!
 ************************************************************************
 * \brief
 *    initializes the current macroblock
 ************************************************************************
 */
void start_macroblock(Macroblock **currMB, struct img_par *img,int CurrentMBInScanOrder)
{
  int mb_nr = img->current_mb_nr;
  *currMB = &img->mb_data[mb_nr];   // intialization code deleted, see below, StW  
  (*currMB)->mbAddrX = mb_nr;

  assert (mb_nr < (int) img->PicSizeInMbs);

  /* Update coordinates of the current macroblock */
  if (img->MbaffFrameFlag)
  {
    img->mb_x = (mb_nr)%((2*img->width)/MB_BLOCK_SIZE);
    img->mb_y = 2*((mb_nr)/((2*img->width)/MB_BLOCK_SIZE));

    img->mb_y += (img->mb_x & 0x01);
    img->mb_x >>= 1;
  }
  else
  {
    img->mb_x = PicPos[mb_nr][0];
    img->mb_y = PicPos[mb_nr][1];
  }

  /* Define vertical positions */
  img->block_y = img->mb_y * BLOCK_SIZE;      /* luma block position */
  img->block_y_aff = img->block_y;
  img->pix_y   = img->mb_y * MB_BLOCK_SIZE;   /* luma macroblock position */
  img->pix_c_y = img->mb_y * img->mb_cr_size_y; /* chroma macroblock position */

  /* Define horizontal positions */
  img->block_x = img->mb_x * BLOCK_SIZE;      /* luma block position */
  img->pix_x   = img->mb_x * MB_BLOCK_SIZE;   /* luma pixel position */
  img->pix_c_x = img->mb_x * img->mb_cr_size_x; /* chroma pixel position */

  // Save the slice number of this macroblock. When the macroblock below
  // is coded it will use this to decide if prediction for above is possible
  (*currMB)->slice_nr = img->current_slice_nr;

  if (img->current_slice_nr >= MAX_NUM_SLICES)
  {
    error ("maximum number of supported slices exceeded, please recompile with increased value for MAX_NUM_SLICES", 200);
  }

  dec_picture->slice_id[img->mb_y][img->mb_x] = img->current_slice_nr;
  dec_picture->max_slice_id = imax(img->current_slice_nr, dec_picture->max_slice_id);

  CheckAvailabilityOfNeighbors(*currMB);

  // Reset syntax element entries in MB struct
  (*currMB)->qp              = img->qp ;
  (*currMB)->mb_type         = 0;
  (*currMB)->delta_quant     = 0;
  (*currMB)->cbp             = 0;
  (*currMB)->cbp_blk         = 0;
  (*currMB)->cbp_blk_CbCr[0] = 0;
  (*currMB)->cbp_blk_CbCr[1] = 0;  
  (*currMB)->c_ipred_mode    = DC_PRED_8; //GB

  memset(&((*currMB)->mvd[0][0][0][0]),0, 2 * BLOCK_MULTIPLE * BLOCK_MULTIPLE * 2 * sizeof(int));
  
  memset((*currMB)->cbp_bits, 0, 3 * sizeof(int64));
  memset((*currMB)->cbp_bits_8x8, 0, 3 * sizeof(int64));

  // initialize img->m7
  memset(&(img->m7[0][0][0]), 0, 3 * MB_PIXELS * sizeof(int));

  // store filtering parameters for this MB
  (*currMB)->LFDisableIdc = img->currentSlice->LFDisableIdc;
  (*currMB)->LFAlphaC0Offset = img->currentSlice->LFAlphaC0Offset;
  (*currMB)->LFBetaOffset = img->currentSlice->LFBetaOffset;

}

/*!
 ************************************************************************
 * \brief
 *    set coordinates of the next macroblock
 *    check end_of_slice condition
 ************************************************************************
 */
Boolean exit_macroblock(struct img_par *img,struct inp_par *inp,int eos_bit)
{
 //! The if() statement below resembles the original code, which tested
  //! img->current_mb_nr == img->PicSizeInMbs.  Both is, of course, nonsense
  //! In an error prone environment, one can only be sure to have a new
  //! picture by checking the tr of the next slice header!

// printf ("exit_macroblock: FmoGetLastMBOfPicture %d, img->current_mb_nr %d\n", FmoGetLastMBOfPicture(), img->current_mb_nr);
  img->num_dec_mb++;

  if (img->num_dec_mb == img->PicSizeInMbs)
  {
    return TRUE;
  }
  // ask for last mb in the slice  UVLC
  else
  {

    img->current_mb_nr = FmoGetNextMBNr (img->current_mb_nr);

    if (img->current_mb_nr == -1)     // End of Slice group, MUST be end of slice
    {
      assert (nal_startcode_follows (img, eos_bit) == TRUE);
      return TRUE;
    }

    if(nal_startcode_follows(img, eos_bit) == FALSE)
      return FALSE;

    if(img->type == I_SLICE  || img->type == SI_SLICE || active_pps->entropy_coding_mode_flag == CABAC)
      return TRUE;
    if(img->cod_counter<=0)
      return TRUE;
    return FALSE;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for P-Frames
 ************************************************************************
 */
void interpret_mb_mode_P(Macroblock *currMB)
{
  const int ICBPTAB[6] = {0,16,32,15,31,47};
  int         mbmode = currMB->mb_type;

#define ZERO_P8x8     (mbmode==5)
#define MODE_IS_P8x8  (mbmode==4 || mbmode==5)
#define MODE_IS_I4x4  (mbmode==6)
#define I16OFFSET     (mbmode-7)
#define MODE_IS_IPCM  (mbmode==31)

  if(mbmode <4)
  {
    currMB->mb_type = mbmode;
    memset(&currMB->b8mode[0],mbmode,4 * sizeof(char));
    memset(&currMB->b8pdir[0], 0, 4 * sizeof(char));
  }
  else if(MODE_IS_P8x8)
  {
    currMB->mb_type = P8x8;
    img->allrefzero = ZERO_P8x8;
  }
  else if(MODE_IS_I4x4)
  {
    currMB->mb_type = I4MB;
    memset(&currMB->b8mode[0],IBLOCK, 4 * sizeof(char));
    memset(&currMB->b8pdir[0],    -1, 4 * sizeof(char));
  }
  else if(MODE_IS_IPCM)
  {
    currMB->mb_type = IPCM;
    currMB->cbp = -1;
    currMB->i16mode = 0;

    memset(&currMB->b8mode[0], 0, 4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1, 4 * sizeof(char));
  }
  else
  {
    currMB->mb_type = I16MB;
    currMB->cbp = ICBPTAB[(I16OFFSET)>>2];
    currMB->i16mode = (I16OFFSET) & 0x03;
    memset(&currMB->b8mode[0], 0, 4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1, 4 * sizeof(char));
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for I-Frames
 ************************************************************************
 */
void interpret_mb_mode_I(Macroblock *currMB)
{
  static const int ICBPTAB[6] = {0,16,32,15,31,47};
  int         mbmode   = currMB->mb_type;

  if (mbmode==0)
  {
    currMB->mb_type = I4MB;
    memset(&currMB->b8mode[0],IBLOCK,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));
  }
  else if(mbmode==25)
  {
    currMB->mb_type=IPCM;
    currMB->cbp= -1;
    currMB->i16mode = 0;

    memset(&currMB->b8mode[0],0,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));
  }
  else
  {
    currMB->mb_type = I16MB;
    currMB->cbp= ICBPTAB[(mbmode-1)>>2];
    currMB->i16mode = (mbmode-1) & 0x03;
    memset(&currMB->b8mode[0], 0, 4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1, 4 * sizeof(char));
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for B-Frames
 ************************************************************************
 */
void interpret_mb_mode_B(Macroblock *currMB)
{
  static const int offset2pdir16x16[12]   = {0, 0, 1, 2, 0,0,0,0,0,0,0,0};
  static const int offset2pdir16x8[22][2] = {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,1},{0,0},{0,1},{0,0},{1,0},
                                             {0,0},{0,2},{0,0},{1,2},{0,0},{2,0},{0,0},{2,1},{0,0},{2,2},{0,0}};
  static const int offset2pdir8x16[22][2] = {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,1},{0,0},{0,1},{0,0},
                                             {1,0},{0,0},{0,2},{0,0},{1,2},{0,0},{2,0},{0,0},{2,1},{0,0},{2,2}};

  const int ICBPTAB[6] = {0,16,32,15,31,47};

  int i, mbmode;
  int mbtype  = currMB->mb_type;

  //--- set mbtype, b8type, and b8pdir ---
  if (mbtype==0)       // direct
  {
    mbmode=0;
    memset(&currMB->b8mode[0],0,4 * sizeof(char));
    memset(&currMB->b8pdir[0],2,4 * sizeof(char));
  }
  else if (mbtype==23) // intra4x4
  {
    mbmode=I4MB;
    memset(&currMB->b8mode[0],IBLOCK,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));
  }
  else if ((mbtype>23) && (mbtype<48) ) // intra16x16
  {
    mbmode=I16MB;
    memset(&currMB->b8mode[0],0,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));

    currMB->cbp     = ICBPTAB[(mbtype-24)>>2];
    currMB->i16mode = (mbtype-24) & 0x03;
  }
  else if (mbtype==22) // 8x8(+split)
  {
    mbmode=P8x8;       // b8mode and pdir is transmitted in additional codewords
  }
  else if (mbtype<4)   // 16x16
  {
    mbmode=1;
    memset(&currMB->b8mode[0], 1,4 * sizeof(char));
    memset(&currMB->b8pdir[0],offset2pdir16x16[mbtype],4 * sizeof(char));
  }
  else if(mbtype==48)
  {
    mbmode=IPCM;
    memset(&currMB->b8mode[0], 0,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));

    currMB->cbp= -1;
    currMB->i16mode = 0;
  }

  else if ((mbtype&0x01)==0) // 16x8
  {
    mbmode=2;
    memset(&currMB->b8mode[0], 2,4 * sizeof(char));
    for(i=0;i<4;i++)
    {
      currMB->b8pdir[i]=offset2pdir16x8 [mbtype][i>>1];
    }
  }
  else
  {
    mbmode=3;
    memset(&currMB->b8mode[0], 3,4 * sizeof(char));
    for(i=0;i<4;i++)
    {
      currMB->b8pdir[i]=offset2pdir8x16 [mbtype][i&0x01];
    }
  }
  currMB->mb_type = mbmode;
}
/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for SI-Frames
 ************************************************************************
 */
void interpret_mb_mode_SI(Macroblock *currMB)
{
  const int ICBPTAB[6] = {0,16,32,15,31,47};
  int         mbmode   = currMB->mb_type;

  if (mbmode==0)
  {
    currMB->mb_type = SI4MB;
    memset(&currMB->b8mode[0],IBLOCK,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));
    img->siblock[img->mb_y][img->mb_x]=1;
  }
  else if (mbmode==1)
  {
    currMB->mb_type = I4MB;
    memset(&currMB->b8mode[0],IBLOCK,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));
  }
  else if(mbmode==26)
  {
    currMB->mb_type=IPCM;
    currMB->cbp= -1;
    currMB->i16mode = 0;
    memset(&currMB->b8mode[0],0,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));
  }

  else
  {
    currMB->mb_type = I16MB;
    currMB->cbp= ICBPTAB[(mbmode-1)>>2];
    currMB->i16mode = (mbmode-2) & 0x03;
    memset(&currMB->b8mode[0],0,4 * sizeof(char));
    memset(&currMB->b8pdir[0],-1,4 * sizeof(char));
  }
}
/*!
 ************************************************************************
 * \brief
 *    init macroblock I and P frames
 ************************************************************************
 */
void init_macroblock(struct img_par *img)
{
  int i,j;

  for(j=img->block_y; j < img->block_y + BLOCK_SIZE; j++)
  {                           // reset vectors and pred. modes
    memset(&dec_picture->mv[LIST_0][j][img->block_x][0], 0, 2 * BLOCK_SIZE * sizeof(short));
    memset(&dec_picture->mv[LIST_1][j][img->block_x][0], 0, 2 * BLOCK_SIZE * sizeof(short));
    memset(&dec_picture->ref_idx[LIST_0][j][img->block_x], -1, BLOCK_SIZE * sizeof(char));
    memset(&dec_picture->ref_idx[LIST_1][j][img->block_x], -1, BLOCK_SIZE * sizeof(char));
    memset(&img->ipredmode[j][img->block_x], DC_PRED, BLOCK_SIZE * sizeof(char));
    for (i=img->block_x;i<img->block_x+BLOCK_SIZE;i++)
    {
      dec_picture->ref_pic_id[LIST_0][j][i] = INT64_MIN;
      dec_picture->ref_pic_id[LIST_1][j][i] = INT64_MIN;
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Sets mode for 8x8 block
 ************************************************************************
 */
void SetB8Mode (struct img_par* img, Macroblock* currMB, int value, int i)
{
  static const int p_v2b8 [ 5] = {4, 5, 6, 7, IBLOCK};
  static const int p_v2pd [ 5] = {0, 0, 0, 0, -1};
  static const int b_v2b8 [14] = {0, 4, 4, 4, 5, 6, 5, 6, 5, 6, 7, 7, 7, IBLOCK};
  static const int b_v2pd [14] = {2, 0, 1, 2, 0, 0, 1, 1, 2, 2, 0, 1, 2, -1};

  if (img->type==B_SLICE)
  {
    currMB->b8mode[i]   = b_v2b8[value];
    currMB->b8pdir[i]   = b_v2pd[value];

  }
  else
  {
    currMB->b8mode[i]   = p_v2b8[value];
    currMB->b8pdir[i]   = p_v2pd[value];
  }

}


void reset_coeffs()
{
  // reset all coeffs

  memset(&img->cof[0][0][0], 0, 3 * MB_PIXELS * sizeof(int));
  
  // CAVLC
  memset(&img->nz_coeff[img->current_mb_nr][0][0],0, BLOCK_SIZE * (BLOCK_SIZE + img->num_blk8x8_uv) * sizeof(int));
}

void field_flag_inference(Macroblock *currMB)
{

  if (currMB->mbAvailA)
  {
    currMB->mb_field = img->mb_data[currMB->mbAddrA].mb_field;
  }
  else
  {
    // check top macroblock pair
    currMB->mb_field = currMB->mbAvailB ? img->mb_data[currMB->mbAddrB].mb_field : 0;
  }
}

void set_chroma_qp(Macroblock* currMB)
{
  int i;
  for (i=0; i<2; i++)
  {
    currMB->qpc[i] = iClip3 ( -img->bitdepth_chroma_qp_scale, 51, currMB->qp + dec_picture->chroma_qp_offset[i] );
    currMB->qpc[i] = currMB->qpc[i] < 0 ? currMB->qpc[i] : QP_SCALE_CR[currMB->qpc[i]];
  }
}


/*!
 ************************************************************************
 * \brief
 *    Get the syntax elements from the NAL
 ************************************************************************
 */
void read_one_macroblock(Macroblock *currMB, struct img_par *img,struct inp_par *inp)
{
  int i;

  SyntaxElement currSE;
  int mb_nr = img->current_mb_nr;

  Slice *currSlice = img->currentSlice;
  DataPartition *dP;
  int *partMap = assignSE2partition[currSlice->dp_mode];
  Macroblock *topMB = NULL;
  int  prevMbSkipped = 0;
  int  img_block_y;
  int  check_bottom, read_bottom, read_top;
  

  if (img->MbaffFrameFlag)
  {
    if (mb_nr&0x01)
    {
      topMB= &img->mb_data[mb_nr-1];
      if(!(img->type == B_SLICE))
        prevMbSkipped = (topMB->mb_type == 0);
      else
        prevMbSkipped = topMB->skip_flag;
    }
    else
      prevMbSkipped = 0;
  }

  currMB->mb_field = ((mb_nr&0x01) == 0)? 0 : img->mb_data[mb_nr-1].mb_field;


  currMB->qp = img->qp ;
  for (i=0; i<2; i++)
  {
    currMB->qpc[i] = iClip3 ( -img->bitdepth_chroma_qp_scale, 51, img->qp + dec_picture->chroma_qp_offset[i] );
    currMB->qpc[i] = currMB->qpc[i] < 0 ? currMB->qpc[i] : QP_SCALE_CR[currMB->qpc[i]];
  }

  currSE.type = SE_MBTYPE;

  //  read MB mode *****************************************************************
  dP = &(currSlice->partArr[partMap[currSE.type]]);

  if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)   currSE.mapping = linfo_ue;

  if(img->type == I_SLICE || img->type == SI_SLICE)
  {
    // read MB aff
    if (img->MbaffFrameFlag && (mb_nr&0x01)==0)
    {
      TRACE_STRING("mb_field_decoding_flag");
      if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
      {
        currSE.len = (int64) 1;
        readSyntaxElement_FLC(&currSE, dP->bitstream);
      }
      else
      {
        currSE.reading = readFieldModeInfo_CABAC;
        dP->readSyntaxElement(&currSE,img,dP);
      }
      currMB->mb_field = currSE.value1;
    }
    if(active_pps->entropy_coding_mode_flag  == CABAC)
      CheckAvailabilityOfNeighborsCABAC(currMB);

    //  read MB type
    TRACE_STRING("mb_type");
    currSE.reading = readMB_typeInfo_CABAC;
    dP->readSyntaxElement(&currSE,img,dP);

    currMB->mb_type = currSE.value1;
    if(!dP->bitstream->ei_flag)
      currMB->ei_flag = 0;
  }
  // non I/SI-slice CABAC
  else if (active_pps->entropy_coding_mode_flag == CABAC)
  {
    // read MB skip_flag
    if (img->MbaffFrameFlag && ((mb_nr&0x01) == 0||prevMbSkipped))
      field_flag_inference(currMB);

    CheckAvailabilityOfNeighborsCABAC(currMB);
    TRACE_STRING("mb_skip_flag");
    currSE.reading = readMB_skip_flagInfo_CABAC;
    dP->readSyntaxElement(&currSE,img,dP);

    currMB->mb_type   = currSE.value1;
    currMB->skip_flag = !(currSE.value1);

    if (img->type==B_SLICE)
      currMB->cbp = currSE.value2;

    if(!dP->bitstream->ei_flag)
      currMB->ei_flag = 0;

    if ((img->type==B_SLICE) && currSE.value1==0 && currSE.value2==0)
      img->cod_counter=0;

    // read MB AFF
    if (img->MbaffFrameFlag)
    {
      check_bottom=read_bottom=read_top=0;
      if ((mb_nr&0x01)==0)
      {
        check_bottom =  currMB->skip_flag;
        read_top = !check_bottom;
      }
      else
      {
        read_bottom = (topMB->skip_flag && (!currMB->skip_flag));
       }

      if (read_bottom || read_top)
      {
        TRACE_STRING("mb_field_decoding_flag");
        currSE.reading = readFieldModeInfo_CABAC;
        dP->readSyntaxElement(&currSE,img,dP);
        currMB->mb_field = currSE.value1;
      }
      if (check_bottom)
        check_next_mb_and_get_field_mode_CABAC(&currSE,img,dP);

    }

    CheckAvailabilityOfNeighborsCABAC(currMB);

    // read MB type
    if (currMB->mb_type != 0 )
    {
      currSE.reading = readMB_typeInfo_CABAC;
      TRACE_STRING("mb_type");
      dP->readSyntaxElement(&currSE,img,dP);
      currMB->mb_type = currSE.value1;
      if(!dP->bitstream->ei_flag)
        currMB->ei_flag = 0;
    }
  }
  // VLC Non-Intra
  else
  {
    if(img->cod_counter == -1)
    {
      TRACE_STRING("mb_skip_run");
      dP->readSyntaxElement(&currSE,img,dP);
      img->cod_counter = currSE.value1;
    }
    if (img->cod_counter==0)
    {
      // read MB aff
      if ((img->MbaffFrameFlag) && (((mb_nr&0x01)==0) || ((mb_nr&0x01) && prevMbSkipped)))
      {
        TRACE_STRING("mb_field_decoding_flag");
        currSE.len = (int64) 1;
        readSyntaxElement_FLC(&currSE, dP->bitstream);
        currMB->mb_field = currSE.value1;
      }

      // read MB type
      TRACE_STRING("mb_type");
      dP->readSyntaxElement(&currSE,img,dP);
      if(img->type == P_SLICE || img->type == SP_SLICE)
        currSE.value1++;
      currMB->mb_type = currSE.value1;
      if(!dP->bitstream->ei_flag)
        currMB->ei_flag = 0;
      img->cod_counter--;
      currMB->skip_flag = 0;
    }
    else
    {
      img->cod_counter--;
      currMB->mb_type = 0;
      currMB->ei_flag = 0;
      currMB->skip_flag = 1;

      // read field flag of bottom block
      if(img->MbaffFrameFlag)
      {
        if(img->cod_counter == 0 && ((mb_nr&0x01) == 0))
        {
          TRACE_STRING("mb_field_decoding_flag (of coded bottom mb)");
          currSE.len = (int64) 1;
          readSyntaxElement_FLC(&currSE, dP->bitstream);
          dP->bitstream->frame_bitoffset--;
          currMB->mb_field = currSE.value1;
        }
        else if(img->cod_counter > 0 && ((mb_nr&0x01) == 0))
        {
          // check left macroblock pair first
          if (mb_is_available(mb_nr-2, currMB)&&((mb_nr%(img->PicWidthInMbs*2))!=0))
          {
            currMB->mb_field = img->mb_data[mb_nr-2].mb_field;
          }
          else
          {
            // check top macroblock pair
            if (mb_is_available(mb_nr - 2*img->PicWidthInMbs, currMB))
            {
              currMB->mb_field = img->mb_data[mb_nr-2*img->PicWidthInMbs].mb_field;
            }
            else
              currMB->mb_field = 0;
          }
        }
      }
    }
  }

  dec_picture->mb_field[mb_nr] = currMB->mb_field;

  img->block_y_aff = ((img->MbaffFrameFlag) && (currMB->mb_field)) ? (mb_nr&0x01) ? (img->block_y - 4)>>1 : img->block_y >> 1 : img->block_y;

  img->siblock[img->mb_y][img->mb_x]=0;

  switch (img->type)
  {
  case P_SLICE: case SP_SLICE:
    interpret_mb_mode_P(currMB);
    break;
  case B_SLICE:
    interpret_mb_mode_B(currMB);
    break;
  case I_SLICE: 
    interpret_mb_mode_I(currMB);
    break;
  case SI_SLICE: 
    interpret_mb_mode_SI(currMB);
    break;
  default:
    printf("Unsupported slice type\n");
    break;
  }

  if(img->MbaffFrameFlag)
  {
    if(currMB->mb_field)
    {
      img->num_ref_idx_l0_active <<=1;
      img->num_ref_idx_l1_active <<=1;
    }
  }

  //init NoMbPartLessThan8x8Flag
  currMB->NoMbPartLessThan8x8Flag = (IS_DIRECT(currMB) && !(active_sps->direct_8x8_inference_flag))? 0: 1;

  //====== READ 8x8 SUB-PARTITION MODES (modes of 8x8 blocks) and Intra VBST block modes ======
  if (IS_P8x8 (currMB))
  {
    currSE.type    = SE_MBTYPE;
    dP = &(currSlice->partArr[partMap[SE_MBTYPE]]);

    for (i=0; i<4; i++)
    {
      if (active_pps->entropy_coding_mode_flag ==UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo_ue;
      else                                                  currSE.reading = readB8_typeInfo_CABAC;

      TRACE_STRING("sub_mb_type");
      dP->readSyntaxElement (&currSE, img, dP);
      SetB8Mode (img, currMB, currSE.value1, i);

      //set NoMbPartLessThan8x8Flag for P8x8 mode
      currMB->NoMbPartLessThan8x8Flag &= (currMB->b8mode[i]==0 && active_sps->direct_8x8_inference_flag) ||
                                         (currMB->b8mode[i]==4);
    }
    //--- init macroblock data ---
    init_macroblock       (img);
    readMotionInfoFromNAL (currMB, img, inp);
  }


  //============= Transform Size Flag for INTRA MBs =============
  //-------------------------------------------------------------
  //transform size flag for INTRA_4x4 and INTRA_8x8 modes
  if (currMB->mb_type == I4MB && img->Transform8x8Mode)
  {
    currSE.type   =  SE_HEADER;
    dP = &(currSlice->partArr[partMap[SE_HEADER]]);
    currSE.reading = readMB_transform_size_flag_CABAC;
    TRACE_STRING("transform_size_8x8_flag");

    // read UVLC transform_size_8x8_flag
    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
    {
      currSE.len = (int64) 1;
      readSyntaxElement_FLC(&currSE, dP->bitstream);
    }
    else
    {
      dP->readSyntaxElement(&currSE,img,dP);
    }

    currMB->luma_transform_size_8x8_flag = currSE.value1;

    if (currMB->luma_transform_size_8x8_flag)
    {
      currMB->mb_type = I8MB;
      for (i=0;i<4;i++)
      {
        currMB->b8mode[i]=I8MB;
        currMB->b8pdir[i]=-1;
      }
    }
  }
  else
  {
    currMB->luma_transform_size_8x8_flag = 0;
  }

  if(active_pps->constrained_intra_pred_flag && (img->type==P_SLICE|| img->type==B_SLICE))        // inter frame
  {
    if( !IS_INTRA(currMB) )
    {
      img->intra_block[mb_nr] = 0;
    }
  }

  //! TO for error concealment
  //! If we have an INTRA Macroblock and we lost the partition
  //! which contains the intra coefficients Copy MB would be better
  //! than just a gray block.
  //! Seems to be a bit at the wrong place to do this right here, but for this case
  //! up to now there is no other way.
/*
 !!!KS
  dP = &(currSlice->partArr[partMap[SE_CBP_INTRA]]);
  if(IS_INTRA (currMB) && dP->bitstream->ei_flag && img->number)
  {
    currMB->mb_type = 0;
    currMB->ei_flag = 1;
    for (i=0;i<4;i++) {currMB->b8mode[i]=currMB->b8pdir[i]=0; }
  }
  dP = &(currSlice->partArr[partMap[currSE.type]]);
  //! End TO
*/

  //--- init macroblock data ---
  if (!IS_P8x8 (currMB))
    init_macroblock(img);

  if (IS_DIRECT (currMB) && img->cod_counter >= 0)
  {
    currMB->cbp = 0;
    reset_coeffs();

    if (active_pps->entropy_coding_mode_flag ==CABAC)
      img->cod_counter=-1;

    return;
  }

  if (IS_COPY (currMB)) //keep last macroblock
  {
    int i, j;
    short pmv[2];
    int zeroMotionAbove;
    int zeroMotionLeft;
    PixelPos mb_a, mb_b;
    int      a_mv_y = 0;
    int      a_ref_idx = 0;
    int      b_mv_y = 0;
    int      b_ref_idx = 0;
    int      list_offset = ((img->MbaffFrameFlag)&&(currMB->mb_field))? (mb_nr&0x01) ? 4 : 2 : 0;
    short ***cur_mv = dec_picture->mv[LIST_0];
    getLuma4x4Neighbour(currMB,-1, 0, &mb_a);
    getLuma4x4Neighbour(currMB, 0,-1, &mb_b);

    if (mb_a.available)
    {
      a_mv_y    = cur_mv[mb_a.pos_y][mb_a.pos_x][1];
      a_ref_idx = dec_picture->ref_idx[LIST_0][mb_a.pos_y][mb_a.pos_x];

      if (currMB->mb_field && !img->mb_data[mb_a.mb_addr].mb_field)
      {
        a_mv_y    /=2;
        a_ref_idx *=2;
      }
      if (!currMB->mb_field && img->mb_data[mb_a.mb_addr].mb_field)
      {
        a_mv_y    *=2;
        a_ref_idx >>=1;
      }
    }

    if (mb_b.available)
    {
      b_mv_y    = cur_mv[mb_b.pos_y][mb_b.pos_x][1];
      b_ref_idx = dec_picture->ref_idx[LIST_0][mb_b.pos_y][mb_b.pos_x];

      if (currMB->mb_field && !img->mb_data[mb_b.mb_addr].mb_field)
      {
        b_mv_y    /=2;
        b_ref_idx *=2;
      }
      if (!currMB->mb_field && img->mb_data[mb_b.mb_addr].mb_field)
      {
        b_mv_y    *=2;
        b_ref_idx >>=1;
      }
    }

    zeroMotionLeft  = !mb_a.available ? 1 : a_ref_idx==0 && cur_mv[mb_a.pos_y][mb_a.pos_x][0]==0 && a_mv_y==0 ? 1 : 0;
    zeroMotionAbove = !mb_b.available ? 1 : b_ref_idx==0 && cur_mv[mb_b.pos_y][mb_b.pos_x][0]==0 && b_mv_y==0 ? 1 : 0;

    currMB->cbp = 0;
    reset_coeffs();

    img_block_y   = img->block_y;

    if (zeroMotionAbove || zeroMotionLeft)
    {
      for(j=img_block_y;j<img_block_y + BLOCK_SIZE;j++)
      {
        memset(&cur_mv[j][img->block_x][0], 0, 2 * BLOCK_SIZE * sizeof(short));
      }
    }
    else
    {
      SetMotionVectorPredictor (currMB, img, pmv, 0, LIST_0, dec_picture->ref_idx, dec_picture->mv, 0, 0, 16, 16);

      for(j=img_block_y;j<img_block_y + BLOCK_SIZE;j++)
      {        
        for(i=img->block_x;i<img->block_x + BLOCK_SIZE;i++)
          memcpy(&cur_mv[j][i][0], &pmv[0], 2 * sizeof(short));
      }
    }
    for(j=img_block_y;j< img_block_y + BLOCK_SIZE;j++)
    {
      memset(&dec_picture->ref_idx[LIST_0][j][img->block_x], 0, BLOCK_SIZE * sizeof(char));
      for(i=img->block_x;i<img->block_x + BLOCK_SIZE;i++)
      {        
        dec_picture->ref_pic_id[LIST_0][j][i] =
          dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][(short)dec_picture->ref_idx[LIST_0][j][i]];
      }
    }
    return;
  }
  if(currMB->mb_type!=IPCM)
  {

    // intra prediction modes for a macroblock 4x4 **********************************************
    read_ipred_modes(currMB, img,inp);

    // read inter frame vector data *********************************************************
    if (IS_INTERMV (currMB) && (!IS_P8x8(currMB)))
    {
      readMotionInfoFromNAL (currMB, img, inp);
    }
    // read CBP and Coeffs  ***************************************************************
    readCBPandCoeffsFromNAL (currMB, img, inp);
  }
  else
  {
    //read pcm_alignment_zero_bit and pcm_byte[i]

    // here dP is assigned with the same dP as SE_MBTYPE, because IPCM syntax is in the
    // same category as MBTYPE
    if ( currSlice->dp_mode && currSlice->dpB_NotPresent )
    {
      concealIPCMcoeffs(img);
    }
    else
    {
      dP = &(currSlice->partArr[partMap[SE_LUM_DC_INTRA]]);
      readIPCMcoeffsFromNAL(img,inp,dP);
    }
  }

  return;
}


/*!
 ************************************************************************
 * \brief
 *    Initialize decoding engine after decoding an IPCM macroblock
 *    (for IPCM CABAC  28/11/2003)
 *
 * \author
 *    Dong Wang <Dong.Wang@bristol.ac.uk>
 ************************************************************************
 */
void init_decoding_engine_IPCM(struct img_par *img)
{
  Slice *currSlice = img->currentSlice;
  Bitstream *currStream;
  int ByteStartPosition;
  int PartitionNumber;
  int i;

  if(currSlice->dp_mode==PAR_DP_1)
    PartitionNumber=1;
  else if(currSlice->dp_mode==PAR_DP_3)
    PartitionNumber=3;
  else
  {
    printf("Partition Mode is not supported\n");
    exit(1);
  }

  for(i=0;i<PartitionNumber;i++)
  {
    currStream = currSlice->partArr[i].bitstream;
    ByteStartPosition = currStream->read_len;

    arideco_start_decoding (&currSlice->partArr[i].de_cabac, currStream->streamBuffer, ByteStartPosition, &currStream->read_len, img->type);
  }
}




/*!
 ************************************************************************
 * \brief
 *    Read IPCM pcm_alignment_zero_bit and pcm_byte[i] from stream to img->cof
 *    (for IPCM CABAC and IPCM CAVLC)
 *
 * \author
 *    Dong Wang <Dong.Wang@bristol.ac.uk>
 ************************************************************************
 */

void readIPCMcoeffsFromNAL(struct img_par *img, struct inp_par *inp, struct datapartition *dP)
{
  SyntaxElement currSE;
  int i,j;

  //For CABAC, we don't need to read bits to let stream byte aligned
  //  because we have variable for integer bytes position
  if(active_pps->entropy_coding_mode_flag  == CABAC)
  {
    //read luma and chroma IPCM coefficients
    currSE.len = (int64) 8;
    TRACE_STRING("pcm_byte luma");

    for(i=0;i<MB_BLOCK_SIZE;i++)
    {
      for(j=0;j<MB_BLOCK_SIZE;j++)
      {
        readIPCMBytes_CABAC(&currSE, dP);
        img->cof[0][i][j] = currSE.value1;        
      }
    }
    if ((dec_picture->chroma_format_idc != YUV400) && !IS_INDEPENDENT(img))
    {
      TRACE_STRING("pcm_byte chroma");
      for(i=0;i<img->mb_cr_size_y;i++)
      {
        for(j=0;j<img->mb_cr_size_x;j++)
        {
          readIPCMBytes_CABAC(&currSE, dP);
          img->cof[1][i][j] = currSE.value1;          
        }
      }
      for(i=0;i<img->mb_cr_size_y;i++)
      {
        for(j=0;j<img->mb_cr_size_x;j++)
        {
          readIPCMBytes_CABAC(&currSE, dP);
          img->cof[2][i][j] = currSE.value1;          
        }
      }
    }
    //If the decoded MB is IPCM MB, decoding engine is initialized

    // here the decoding engine is directly initialized without checking End of Slice
    // The reason is that, whether current MB is the last MB in slice or not, there is
    // at least one 'end of slice' syntax after this MB. So when fetching bytes in this
    // initialization process, we can guarantee there is bits available in bitstream.

    init_decoding_engine_IPCM(img);
  }
  else
  {
    //read bits to let stream byte aligned

    if(((dP->bitstream->frame_bitoffset) & 0x07) != 0)
    {
      TRACE_STRING("pcm_alignment_zero_bit");
      currSE.len = (8 - ((dP->bitstream->frame_bitoffset) & 0x07));
      readSyntaxElement_FLC(&currSE, dP->bitstream);
    }

    //read luma and chroma IPCM coefficients
    currSE.len=img->bitdepth_luma;
    TRACE_STRING("pcm_sample_luma");

    for(i=0;i<MB_BLOCK_SIZE;i++)
    {
      for(j=0;j<MB_BLOCK_SIZE;j++)
      {
        readSyntaxElement_FLC(&currSE, dP->bitstream);
        img->cof[0][i][j] = currSE.value1;
      }
    }
    currSE.len=img->bitdepth_chroma;
    if ((dec_picture->chroma_format_idc != YUV400) && !IS_INDEPENDENT(img))
    {
      TRACE_STRING("pcm_sample_chroma (u)");
      for(i=0;i<img->mb_cr_size_y;i++)
      {
        for(j=0;j<img->mb_cr_size_x;j++)
        {
          readSyntaxElement_FLC(&currSE, dP->bitstream);
          img->cof[1][i][j] = currSE.value1;          
        }
      }
      TRACE_STRING("pcm_sample_chroma (v)");
      for(i=0;i<img->mb_cr_size_y;i++)
      {
        for(j=0;j<img->mb_cr_size_x;j++)
        {
          readSyntaxElement_FLC(&currSE, dP->bitstream);
          img->cof[2][i][j] = currSE.value1;          
        }
      }
    }
  }
}


/*!
************************************************************************
* \brief
*    If data partition B is lost, conceal PCM sample values with DC.
*
************************************************************************
*/

void concealIPCMcoeffs(struct img_par *img)
{
  int i, j, k;

  for(i=0;i<MB_BLOCK_SIZE;i++)
  {
    for(j=0;j<MB_BLOCK_SIZE;j++)
    {
      img->cof[0][i][j] = img->dc_pred_value_comp[0];
    }
  }

  if ((dec_picture->chroma_format_idc != YUV400) && !IS_INDEPENDENT(img))
  {
    for (k = 0; k < 2; k++)
    {
      for(i=0;i<img->mb_cr_size_y;i++)
      {
        for(j=0;j<img->mb_cr_size_x;j++)
        {
          img->cof[k][i][j] = img->dc_pred_value_comp[k];
        }
      }
    }
  }
}



void read_ipred_modes(Macroblock *currMB, struct img_par *img,struct inp_par *inp)
{
  int b8,i,j,bi,bj,bx,by,dec;
  SyntaxElement currSE;
  DataPartition *dP;
  Slice *currSlice = img->currentSlice;
  int *partMap = assignSE2partition[currSlice->dp_mode];
  int ts, ls;
  int mostProbableIntraPredMode;
  int upIntraPredMode;
  int leftIntraPredMode;
  int IntraChromaPredModeFlag = IS_INTRA(currMB);
  int bs_x, bs_y;
  int ii,jj;
  
  PixelPos left_block, top_block;

  currSE.type = SE_INTRAPREDMODE;

  TRACE_STRING("intra4x4_pred_mode");
  dP = &(currSlice->partArr[partMap[currSE.type]]);

  if (!(active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag))
    currSE.reading = readIntraPredMode_CABAC;

  for(b8=0;b8<4;b8++)  //loop 8x8 blocks
  {
    if((currMB->b8mode[b8]==IBLOCK )||(currMB->b8mode[b8]==I8MB))
    {
      bs_x = bs_y = (currMB->b8mode[b8] == I8MB)?8:4;

      IntraChromaPredModeFlag = 1;

      ii=(bs_x>>2);
      jj=(bs_y>>2);

      for(j=0;j<2;j+=jj)  //loop subblocks
      {
        by = (b8&2) + j;
        bj = img->block_y + by;
        for(i=0;i<2;i+=ii)
        {
          bx = ((b8&1)<<1) + i;
          bi = img->block_x + bx;
          //get from stream
          if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
            readSyntaxElement_Intra4x4PredictionMode(&currSE,img,dP);
          else
          {
            currSE.context=(b8<<2)+(j<<1)+i;
            dP->readSyntaxElement(&currSE,img,dP);
          }

          getLuma4x4Neighbour(currMB, (bx<<2) - 1, (by<<2),     &left_block);
          getLuma4x4Neighbour(currMB, (bx<<2),     (by<<2) - 1, &top_block);

          //get from array and decode

          if (active_pps->constrained_intra_pred_flag)
          {
            left_block.available = left_block.available ? img->intra_block[left_block.mb_addr] : 0;
            top_block.available  = top_block.available  ? img->intra_block[top_block.mb_addr]  : 0;
          }

          // !! KS: not sure if the following is still correct...
          ts = ls = 0;   // Check to see if the neighboring block is SI
          if (IS_OLDINTRA(currMB) && img->type == SI_SLICE)           // need support for MBINTLC1
          {
            if (left_block.available)
              if (img->siblock [left_block.pos_y][left_block.pos_x])
                ls=1;

            if (top_block.available)
              if (img->siblock [top_block.pos_y][top_block.pos_x])
                ts=1;
          }

          upIntraPredMode            = (top_block.available  &&(ts == 0)) ? img->ipredmode[top_block.pos_y ][top_block.pos_x ] : -1;
          leftIntraPredMode          = (left_block.available &&(ls == 0)) ? img->ipredmode[left_block.pos_y][left_block.pos_x] : -1;

          mostProbableIntraPredMode  = (upIntraPredMode < 0 || leftIntraPredMode < 0) ? DC_PRED : upIntraPredMode < leftIntraPredMode ? upIntraPredMode : leftIntraPredMode;

          dec = (currSE.value1 == -1) ? mostProbableIntraPredMode : currSE.value1 + (currSE.value1 >= mostProbableIntraPredMode);

          //set
          for(jj = 0; jj < (bs_y >> 2); jj++)   //loop 4x4s in the subblock for 8x8 prediction setting
            memset(&img->ipredmode[bj + jj][bi], dec, (bs_x>>2) * sizeof(char));
        }
      }
    }
  }

  if (IntraChromaPredModeFlag && (dec_picture->chroma_format_idc != YUV400) && (dec_picture->chroma_format_idc != YUV444))
  {
    currSE.type = SE_INTRAPREDMODE;
    TRACE_STRING("intra_chroma_pred_mode");
    dP = &(currSlice->partArr[partMap[currSE.type]]);

    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag) 
      currSE.mapping = linfo_ue;
    else
      currSE.reading = readCIPredMode_CABAC;

    dP->readSyntaxElement(&currSE,img,dP);
    currMB->c_ipred_mode = currSE.value1;

    if (currMB->c_ipred_mode < DC_PRED_8 || currMB->c_ipred_mode > PLANE_8)
    {
      error("illegal chroma intra pred mode!\n", 600);
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    Set motion vector predictor
 ************************************************************************
 */
void SetMotionVectorPredictor (Macroblock *currMB,
                               struct img_par  *img,
                               short           pmv[2],
                               char            ref_frame,
                               byte            list,
                               char            ***refPic,
                               short           ****tmp_mv,
                               int             block_x,
                               int             block_y,
                               int             blockshape_x,
                               int             blockshape_y)
{
  int mb_x  = BLOCK_SIZE * block_x;
  int mb_y  = BLOCK_SIZE * block_y;

  int mv_a, mv_b, mv_c, pred_vec=0;
  int mvPredType, rFrameL, rFrameU, rFrameUR;
  int hv;


  PixelPos block_a, block_b, block_c, block_d;

  getLuma4x4Neighbour(currMB, mb_x - 1,            mb_y,     &block_a);
  getLuma4x4Neighbour(currMB, mb_x,                mb_y - 1, &block_b);
  getLuma4x4Neighbour(currMB, mb_x + blockshape_x, mb_y - 1, &block_c);
  getLuma4x4Neighbour(currMB, mb_x - 1,            mb_y - 1, &block_d);

  if (mb_y > 0)
  {
    if (mb_x < 8)  // first column of 8x8 blocks
    {
      if (mb_y==8)
      {
        if (blockshape_x == 16)      block_c.available  = 0;
      }
      else
      {
        if (mb_x+blockshape_x == 8)  block_c.available  = 0;
      }
    }
    else
    {
      if (mb_x+blockshape_x == 16)   block_c.available  = 0;
    }
  }

  if (!block_c.available)
  {
    block_c = block_d;
  }

  mvPredType = MVPRED_MEDIAN;

  if (!img->MbaffFrameFlag)
  {
    rFrameL  = block_a.available ? refPic[list][block_a.pos_y][block_a.pos_x] : -1;
    rFrameU  = block_b.available ? refPic[list][block_b.pos_y][block_b.pos_x] : -1;
    rFrameUR = block_c.available ? refPic[list][block_c.pos_y][block_c.pos_x] : -1;
  }
  else
  {
    if (img->mb_data[img->current_mb_nr].mb_field)
    {
      rFrameL    = block_a.available    ?
        img->mb_data[block_a.mb_addr].mb_field ?
        refPic[list][block_a.pos_y][block_a.pos_x]:
        refPic[list][block_a.pos_y][block_a.pos_x] * 2:
        -1;
      rFrameU    = block_b.available    ?
        img->mb_data[block_b.mb_addr].mb_field ?
        refPic[list][block_b.pos_y][block_b.pos_x]:
        refPic[list][block_b.pos_y][block_b.pos_x] * 2:
        -1;
      rFrameUR    = block_c.available    ?
        img->mb_data[block_c.mb_addr].mb_field ?
        refPic[list][block_c.pos_y][block_c.pos_x]:
        refPic[list][block_c.pos_y][block_c.pos_x] * 2:
        -1;
    }
    else
    {
      rFrameL    = block_a.available    ?
        img->mb_data[block_a.mb_addr].mb_field ?
        refPic[list][block_a.pos_y][block_a.pos_x] >>1:
        refPic[list][block_a.pos_y][block_a.pos_x] :
        -1;
      rFrameU    = block_b.available    ?
        img->mb_data[block_b.mb_addr].mb_field ?
        refPic[list][block_b.pos_y][block_b.pos_x] >>1:
        refPic[list][block_b.pos_y][block_b.pos_x] :
        -1;
      rFrameUR    = block_c.available    ?
        img->mb_data[block_c.mb_addr].mb_field ?
        refPic[list][block_c.pos_y][block_c.pos_x] >>1:
        refPic[list][block_c.pos_y][block_c.pos_x] :
        -1;
    }
  }


  /* Prediction if only one of the neighbors uses the reference frame
   * we are checking
   */
  if (rFrameL == ref_frame && rFrameU != ref_frame && rFrameUR != ref_frame)       
    mvPredType = MVPRED_L;
  else if(rFrameL != ref_frame && rFrameU == ref_frame && rFrameUR != ref_frame)  
    mvPredType = MVPRED_U;
  else if(rFrameL != ref_frame && rFrameU != ref_frame && rFrameUR == ref_frame)  
    mvPredType = MVPRED_UR;
  // Directional predictions
  if(blockshape_x == 8 && blockshape_y == 16)
  {
    if(mb_x == 0)
    {
      if(rFrameL == ref_frame)
        mvPredType = MVPRED_L;
    }
    else
    {
      if( rFrameUR == ref_frame)
        mvPredType = MVPRED_UR;
    }
  }
  else if(blockshape_x == 16 && blockshape_y == 8)
  {
    if(mb_y == 0)
    {
      if(rFrameU == ref_frame)
        mvPredType = MVPRED_U;
    }
    else
    {
      if(rFrameL == ref_frame)
        mvPredType = MVPRED_L;
    }
  }

  for (hv=0; hv < 2; hv++)
  {
    if (!img->MbaffFrameFlag || hv==0)
    {
      mv_a = block_a.available ? tmp_mv[list][block_a.pos_y][block_a.pos_x][hv] : 0;
      mv_b = block_b.available ? tmp_mv[list][block_b.pos_y][block_b.pos_x][hv] : 0;
      mv_c = block_c.available ? tmp_mv[list][block_c.pos_y][block_c.pos_x][hv] : 0;
    }
    else
    {
      if (img->mb_data[img->current_mb_nr].mb_field)
      {
        mv_a = block_a.available  ? img->mb_data[block_a.mb_addr].mb_field?
          tmp_mv[list][block_a.pos_y][block_a.pos_x][hv]:
          tmp_mv[list][block_a.pos_y][block_a.pos_x][hv] / 2:
          0;
        mv_b = block_b.available  ? img->mb_data[block_b.mb_addr].mb_field?
          tmp_mv[list][block_b.pos_y][block_b.pos_x][hv]:
          tmp_mv[list][block_b.pos_y][block_b.pos_x][hv] / 2:
          0;
        mv_c = block_c.available  ? img->mb_data[block_c.mb_addr].mb_field?
          tmp_mv[list][block_c.pos_y][block_c.pos_x][hv]:
          tmp_mv[list][block_c.pos_y][block_c.pos_x][hv] / 2:
          0;
      }
      else
      {
        mv_a = block_a.available  ? img->mb_data[block_a.mb_addr].mb_field?
          tmp_mv[list][block_a.pos_y][block_a.pos_x][hv] * 2:
          tmp_mv[list][block_a.pos_y][block_a.pos_x][hv]:
          0;
        mv_b = block_b.available  ? img->mb_data[block_b.mb_addr].mb_field?
          tmp_mv[list][block_b.pos_y][block_b.pos_x][hv] * 2:
          tmp_mv[list][block_b.pos_y][block_b.pos_x][hv]:
          0;
        mv_c = block_c.available  ? img->mb_data[block_c.mb_addr].mb_field?
          tmp_mv[list][block_c.pos_y][block_c.pos_x][hv] * 2:
          tmp_mv[list][block_c.pos_y][block_c.pos_x][hv]:
          0;
      }
    }

    switch (mvPredType)
    {
    case MVPRED_MEDIAN:
      if(!(block_b.available || block_c.available))
        pred_vec = mv_a;
      else
        pred_vec = mv_a + mv_b + mv_c - imin(mv_a,imin(mv_b,mv_c))-imax(mv_a,imax(mv_b,mv_c));
      break;
    case MVPRED_L:
      pred_vec = mv_a;
      break;
    case MVPRED_U:
      pred_vec = mv_b;
      break;
    case MVPRED_UR:
      pred_vec = mv_c;
      break;
    default:
      break;
    }

    pmv[hv] = pred_vec;
  }
}


/*!
 ************************************************************************
 * \brief
 *    Set context for reference frames
 ************************************************************************
 */
int BType2CtxRef (int btype)
{
  return (btype < 4 ? 0 : 1);
}

/*!
 ************************************************************************
 * \brief
 *    Read motion info
 ************************************************************************
 */
void readMotionInfoFromNAL (Macroblock *currMB, struct img_par *img, struct inp_par *inp)
{
  int i,j,k;
  int step_h,step_v;
  int curr_mvd;
  int mb_nr = img->current_mb_nr;
  SyntaxElement currSE;
  Slice *currSlice    = img->currentSlice;
  DataPartition *dP;
  int *partMap        = assignSE2partition[currSlice->dp_mode];
  int bframe          = (img->type==B_SLICE);
  int partmode        = (IS_P8x8(currMB)?4:currMB->mb_type);
  int step_h0         = BLOCK_STEP [partmode][0];
  int step_v0         = BLOCK_STEP [partmode][1];

  int mv_mode, i0, j0;
  char refframe;
  short pmv[2];
  int j4, i4, ii,jj;
  int vec;

  int mv_scale = 0;

  int flag_mode;

  int list_offset = ((img->MbaffFrameFlag)&&(currMB->mb_field))? (mb_nr&0x01) ? 4 : 2 : 0;

  byte  **    moving_block;
  short ****  co_located_mv;
  char  ***   co_located_ref_idx;
  int64 ***   co_located_ref_id;

  if ((img->MbaffFrameFlag)&&(currMB->mb_field))
  {
    if(mb_nr&0x01)
    {
      moving_block = Co_located->bottom_moving_block;
      co_located_mv = Co_located->bottom_mv;
      co_located_ref_idx = Co_located->bottom_ref_idx;
      co_located_ref_id = Co_located->bottom_ref_pic_id;
    }
    else
    {
      moving_block = Co_located->top_moving_block;
      co_located_mv = Co_located->top_mv;
      co_located_ref_idx = Co_located->top_ref_idx;
      co_located_ref_id = Co_located->top_ref_pic_id;
    }
  }
  else
  {
    moving_block = Co_located->moving_block;
    co_located_mv = Co_located->mv;
    co_located_ref_idx = Co_located->ref_idx;
    co_located_ref_id = Co_located->ref_pic_id;
  }

  if (bframe && IS_P8x8 (currMB))
  {
    if (img->direct_spatial_mv_pred_flag)
    {
      char  l0_rFrame,l1_rFrame;
      short pmvl0[2]={0,0}, pmvl1[2]={0,0};

      prepare_direct_params(currMB, dec_picture, img, pmvl0, pmvl1, &l0_rFrame, &l1_rFrame);

      for (i=0;i<4;i++)
      {
        if (currMB->b8mode[i] == 0)
        {
          for(j=2*(i>>1);j<2*(i>>1)+2;j++)
          {
            for(k=2*(i&0x01);k<2*(i&0x01)+2;k++)
            {
              int j6 = img->block_y_aff + j;
              j4 = img->block_y+j;
              i4 = img->block_x+k;


              if (l0_rFrame >= 0)
              {
                if  (!l0_rFrame  && ((!moving_block[j6][i4]) && (!listX[LIST_1 + list_offset][0]->is_long_term)))
                {
                  dec_picture->mv  [LIST_0][j4][i4][0] = 0;
                  dec_picture->mv  [LIST_0][j4][i4][1] = 0;
                  dec_picture->ref_idx[LIST_0][j4][i4] = 0;
                }
                else
                {
                  dec_picture->mv  [LIST_0][j4][i4][0] = pmvl0[0];
                  dec_picture->mv  [LIST_0][j4][i4][1] = pmvl0[1];
                  dec_picture->ref_idx[LIST_0][j4][i4] = l0_rFrame;
                }
              }
              else
              {
                dec_picture->mv  [LIST_0][j4][i4][0] = 0;
                dec_picture->mv  [LIST_0][j4][i4][1] = 0;
                dec_picture->ref_idx[LIST_0][j4][i4] = -1;
              }

              if (l1_rFrame >= 0)
              {
                if  (l1_rFrame==0 && ((!moving_block[j6][i4])&& (!listX[LIST_1 + list_offset][0]->is_long_term)))
                {
                  dec_picture->mv  [LIST_1][j4][i4][0] = 0;
                  dec_picture->mv  [LIST_1][j4][i4][1] = 0;
                  dec_picture->ref_idx[LIST_1][j4][i4] = 0;
                }
                else
                {
                  dec_picture->mv  [LIST_1][j4][i4][0] = pmvl1[0];
                  dec_picture->mv  [LIST_1][j4][i4][1] = pmvl1[1];
                  dec_picture->ref_idx[LIST_1][j4][i4] = l1_rFrame;
                }
              }
              else
              {
                dec_picture->mv  [LIST_1][j4][i4][0] = 0;
                dec_picture->mv  [LIST_1][j4][i4][1] = 0;
                dec_picture->ref_idx[LIST_1][j4][i4] = -1;
              }

              if (l0_rFrame <0 && l1_rFrame <0)
              {
                dec_picture->ref_idx[LIST_0][j4][i4] = 0;
                dec_picture->ref_idx[LIST_1][j4][i4] = 0;
              }
            }
          }
        }
      }
    }
    else
    {
      for (i=0;i<4;i++)
      {
        if (currMB->b8mode[i] == 0)
        {
          for(j = 2 * (i >> 1); j < 2 * (i >> 1) + 2;j++)
          {
            for(k=2*(i&0x01);k<2*(i&0x01)+2;k++)
            {

              int list_offset = ((img->MbaffFrameFlag)&&(currMB->mb_field))? (mb_nr&0x01) ? 4 : 2 : 0;
              
              int refList = co_located_ref_idx[LIST_0 ][img->block_y_aff+j][img->block_x+k]== -1 ? LIST_1 : LIST_0;
              int ref_idx = co_located_ref_idx[refList][img->block_y_aff + j][img->block_x + k];
              int mapped_idx=-1, iref;

              if (ref_idx == -1)
              {
                dec_picture->ref_idx [LIST_0][img->block_y + j][img->block_x + k] = 0;
                dec_picture->ref_idx [LIST_1][img->block_y + j][img->block_x + k] = 0;
              }
              else
              {
                for (iref=0;iref<imin(img->num_ref_idx_l0_active,listXsize[LIST_0 + list_offset]);iref++)
                {
                  int curr_mb_field = ((img->MbaffFrameFlag)&&(currMB->mb_field));

                  if(img->structure==0 && curr_mb_field==0)
                  {
                    // If the current MB is a frame MB and the colocated is from a field picture,
                    // then the co_located_ref_id may have been generated from the wrong value of
                    // frame_poc if it references it's complementary field, so test both POC values
                    if(listX[0][iref]->top_poc*2 == co_located_ref_id[refList][img->block_y_aff + j][img->block_x + k]
                    || listX[0][iref]->bottom_poc*2 == co_located_ref_id[refList][img->block_y_aff + j][img->block_x + k])
                    {
                      mapped_idx=iref;
                      break;
                    }
                    else //! invalid index. Default to zero even though this case should not happen
                      mapped_idx=INVALIDINDEX;
                    continue;
                  }
                  if (dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][iref]==co_located_ref_id[refList][img->block_y_aff + j][img->block_x + k])
                  {
                    mapped_idx=iref;
                    break;
                  }
                  else //! invalid index. Default to zero even though this case should not happen
                    mapped_idx=INVALIDINDEX;
                }
                if (INVALIDINDEX == mapped_idx)
                {
                  error("temporal direct error\ncolocated block has ref that is unavailable",-1111);
                }
                dec_picture->ref_idx [LIST_0][img->block_y + j][img->block_x + k] = mapped_idx;
                dec_picture->ref_idx [LIST_1][img->block_y + j][img->block_x + k] = 0;
              }
            }
          }
        }
      }
    }
  }

  //  If multiple ref. frames, read reference frame for the MB *********************************
  if(img->num_ref_idx_l0_active>1)
  {
    flag_mode = ( img->num_ref_idx_l0_active == 2 ? 1 : 0);

    currSE.type = SE_REFFRAME;
    dP = &(currSlice->partArr[partMap[SE_REFFRAME]]);

    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)   currSE.mapping = linfo_ue;
    else                                                      currSE.reading = readRefFrame_CABAC;

    for (j0=0; j0<4; j0+=step_v0)
    {
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=2*(j0>>1)+(i0>>1);
        if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)
        {
          TRACE_STRING("ref_idx_l0");

          img->subblock_x = i0;
          img->subblock_y = j0;

          if (!IS_P8x8 (currMB) || bframe || (!bframe && !img->allrefzero))
          {
            currSE.context = BType2CtxRef (currMB->b8mode[k]);
            if( (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag) && flag_mode )
            {
              currSE.len = 1;
              readSyntaxElement_FLC(&currSE, dP->bitstream);
              currSE.value1 = 1 - currSE.value1;
            }
            else
            {
              currSE.value2 = LIST_0;
              dP->readSyntaxElement (&currSE,img,dP);
            }
            refframe = currSE.value1;
          }
          else
          {
            refframe = 0;
          }

          for (j=img->block_y +j0; j<img->block_y +j0+step_v0;j++)
            memset(&dec_picture->ref_idx[LIST_0][j][img->block_x + i0], refframe, step_h0 * sizeof(char));
        }
      }
    }
  }
  else
  {
    for (j0=0; j0<4; j0+=step_v0)
    {
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=2*(j0>>1)+(i0>>1);
        if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)
        {
          for (j=img->block_y + j0; j < img->block_y + j0+step_v0;j++)
            memset(&dec_picture->ref_idx[LIST_0][j][img->block_x + i0], 0, step_h0 * sizeof(char));
        }
      }
    }
  }

  //  If backward multiple ref. frames, read backward reference frame for the MB *********************************
  if(img->num_ref_idx_l1_active>1)
  {
    flag_mode = ( img->num_ref_idx_l1_active == 2 ? 1 : 0);

    currSE.type = SE_REFFRAME;
    dP = &(currSlice->partArr[partMap[SE_REFFRAME]]);
    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
      currSE.mapping = linfo_ue;
    else
      currSE.reading = readRefFrame_CABAC;

    for (j0=0; j0<4; j0+=step_v0)
    {
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=2*(j0>>1)+(i0>>1);
        if ((currMB->b8pdir[k]==1 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)
        {
          TRACE_STRING("ref_idx_l1");

          img->subblock_x = i0;
          img->subblock_y = j0;

          currSE.context = BType2CtxRef (currMB->b8mode[k]);
          if( (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag) && flag_mode )
          {
            currSE.len = 1;
            readSyntaxElement_FLC(&currSE, dP->bitstream);
            currSE.value1 = 1-currSE.value1;
          }
          else
          {
            currSE.value2 = LIST_1;
            dP->readSyntaxElement (&currSE,img,dP);
          }
          refframe = currSE.value1;

          for (j=img->block_y + j0; j<img->block_y + j0+step_v0;j++)
          {
            memset(&dec_picture->ref_idx[LIST_1][j][img->block_x + i0], refframe, step_h0 * sizeof(char));
          }
        }
      }
    }
  }
  else
  {
    for (j0=0; j0<4; j0+=step_v0)
    {
      for (i0=0; i0<4; i0+=step_h0)
      {
        k=2*(j0>>1)+(i0>>1);
        if ((currMB->b8pdir[k]==1 || currMB->b8pdir[k]==2) && currMB->b8mode[k]!=0)
        {
          for (j=img->block_y + j0; j<img->block_y + j0+step_v0;j++)
            memset(&dec_picture->ref_idx[LIST_1][ j][img->block_x + i0], 0, step_h0 * sizeof(char));
        }
      }
    }
  }

  //=====  READ FORWARD MOTION VECTORS =====
  currSE.type = SE_MVD;
  dP = &(currSlice->partArr[partMap[SE_MVD]]);

  if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo_se;
  else                                                  currSE.reading = readMVD_CABAC;

  for (j0=0; j0<4; j0+=step_v0)
    for (i0=0; i0<4; i0+=step_h0)
    {
      k=2*(j0>>1)+(i0>>1);

      if ((currMB->b8pdir[k]==0 || currMB->b8pdir[k]==2) && (currMB->b8mode[k] !=0))//has forward vector
      {
        mv_mode  = currMB->b8mode[k];
        step_h   = BLOCK_STEP [mv_mode][0];
        step_v   = BLOCK_STEP [mv_mode][1];

        refframe = dec_picture->ref_idx[LIST_0][img->block_y+j0][img->block_x+i0];

        for (j=j0; j<j0+step_v0; j+=step_v)
        {
          j4 = img->block_y+j;
          for (i=i0; i<i0+step_h0; i+=step_h)
          {
            i4 = img->block_x+i;

            // first make mv-prediction
            SetMotionVectorPredictor (currMB, img, pmv, refframe, LIST_0, dec_picture->ref_idx, dec_picture->mv, i, j, 4*step_h, 4*step_v);

            for (k=0; k < 2; k++)
            {
              TRACE_STRING("mvd_l0");

              img->subblock_x = i; // position used for context determination
              img->subblock_y = j; // position used for context determination
              currSE.value2 = k<<1; // identifies the component; only used for context determination
              dP->readSyntaxElement(&currSE,img,dP);
              curr_mvd = currSE.value1;

              vec=curr_mvd+pmv[k];           /* find motion vector */

              for(jj=0;jj<step_v;jj++)
              {
                for(ii=0;ii<step_h;ii++)
                {
                  dec_picture->mv [LIST_0][j4+jj][i4+ii][k] = vec;
                  currMB->mvd     [LIST_0][j +jj][i +ii][k] = curr_mvd;
                }
              }
            }
          }
        }
      }
      else if (currMB->b8mode[k=2*(j0>>1)+(i0>>1)]==0)
      {
        if (!img->direct_spatial_mv_pred_flag)
        {
          int list_offset = ((img->MbaffFrameFlag)&&(currMB->mb_field))? (mb_nr&0x01) ? 4 : 2 : 0;          

          int refList = (co_located_ref_idx[LIST_0 ][img->block_y_aff+j0][img->block_x+i0]== -1 ? LIST_1 : LIST_0);
          int ref_idx =  co_located_ref_idx[refList][img->block_y_aff+j0][img->block_x+i0];

          if (ref_idx==-1)
          {
            for (j4=img->block_y+j0; j4<img->block_y+j0+step_v0; j4++)
            {
              memset(&dec_picture->ref_idx [LIST_0][j4][img->block_x+i0],0, step_h0 * sizeof(char));
              memset(&dec_picture->ref_idx [LIST_1][j4][img->block_x+i0],0, step_h0 * sizeof(char));
              for (i4=img->block_x+i0; i4<img->block_x+i0+step_h0; i4++)
              {
                memset(&dec_picture->mv [LIST_0][j4][i4][0], 0, 2 * sizeof(short));
                memset(&dec_picture->mv [LIST_1][j4][i4][0], 0, 2 * sizeof(short));
              }
            }
          }
          else
          {
            int mapped_idx=-1, iref;
            int j6;

            for (iref = 0; iref < imin(img->num_ref_idx_l0_active, listXsize[LIST_0 + list_offset]); iref++)
            {
              int curr_mb_field = ((img->MbaffFrameFlag)&&(currMB->mb_field));

              if(img->structure==0 && curr_mb_field==0)
              {
                // If the current MB is a frame MB and the colocated is from a field picture,
                // then the co_located_ref_id may have been generated from the wrong value of
                // frame_poc if it references it's complementary field, so test both POC values
                if(listX[0][iref]->top_poc * 2    == co_located_ref_id[refList][img->block_y_aff + j0][img->block_x + i0]
                || listX[0][iref]->bottom_poc * 2 == co_located_ref_id[refList][img->block_y_aff + j0][img->block_x + i0])
                {
                  mapped_idx=iref;
                  break;
                }
                else //! invalid index. Default to zero even though this case should not happen
                  mapped_idx=INVALIDINDEX;
                continue;
              }
              if (dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][iref]==co_located_ref_id[refList][img->block_y_aff+j0][img->block_x+i0])
              {
                mapped_idx=iref;
                break;
              }
              else //! invalid index. Default to zero even though this case should not happen
                mapped_idx=INVALIDINDEX;
            }

            if (INVALIDINDEX == mapped_idx)
            {
              error("temporal direct error\ncolocated block has ref that is unavailable",-1111);
            }

            for (j=j0; j<j0+step_v0; j++)
            {
              j4 = img->block_y+j;
              j6 = img->block_y_aff + j;

              for (i4=img->block_x+i0; i4<img->block_x+i0+step_h0; i4++)
              {                
                mv_scale = img->mvscale[LIST_0 + list_offset][mapped_idx];

                dec_picture->ref_idx [LIST_0][j4][i4] = mapped_idx;
                dec_picture->ref_idx [LIST_1][j4][i4] = 0;

                for (ii=0; ii < 2; ii++)
                {
                  if (mv_scale == 9999 || listX[LIST_0+list_offset][mapped_idx]->is_long_term)
                  {
                    dec_picture->mv  [LIST_0][j4][i4][ii] = co_located_mv[refList][j6][i4][ii];
                    dec_picture->mv  [LIST_1][j4][i4][ii] = 0;
                  }
                  else
                  {
                    dec_picture->mv  [LIST_0][j4][i4][ii] = (mv_scale * co_located_mv[refList][j6][i4][ii] + 128 ) >> 8;
                    dec_picture->mv  [LIST_1][j4][i4][ii] = dec_picture->mv[LIST_0][j4][i4][ii] - co_located_mv[refList][j6][i4][ii];
                  }
                }                
              }
            }
          }
        }
      }
    }

  //=====  READ BACKWARD MOTION VECTORS =====
  currSE.type = SE_MVD;
  dP          = &(currSlice->partArr[partMap[SE_MVD]]);

  if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag) currSE.mapping = linfo_se;
  else                                                    currSE.reading = readMVD_CABAC;

  for (j0=0; j0<4; j0+=step_v0)
  {
    for (i0=0; i0<4; i0+=step_h0)
    {
      k=2*(j0>>1)+(i0>>1);
      if ((currMB->b8pdir[k]==1 || currMB->b8pdir[k]==2) && (currMB->b8mode[k]!=0))//has backward vector
      {
        mv_mode  = currMB->b8mode[k];
        step_h   = BLOCK_STEP [mv_mode][0];
        step_v   = BLOCK_STEP [mv_mode][1];

        refframe = dec_picture->ref_idx[LIST_1][img->block_y+j0][img->block_x+i0];

        for (j=j0; j<j0+step_v0; j+=step_v)
        {
          j4 = img->block_y+j;
          for (i=i0; i<i0+step_h0; i+=step_h)
          {
            i4 = img->block_x+i;

            // first make mv-prediction
            SetMotionVectorPredictor (currMB, img, pmv, refframe, LIST_1, dec_picture->ref_idx, dec_picture->mv, i, j, 4*step_h, 4*step_v);

            for (k=0; k < 2; k++)
            {
              TRACE_STRING("mvd_l1");

              img->subblock_x = i; // position used for context determination
              img->subblock_y = j; // position used for context determination
              currSE.value2   = (k<<1) +1; // identifies the component; only used for context determination
              dP->readSyntaxElement(&currSE,img,dP);
              curr_mvd = currSE.value1;

              vec=curr_mvd+pmv[k];           /* find motion vector */

              for(jj=0;jj<step_v;jj++)
              {
                for(ii=0;ii<step_h;ii++)
                {
                  dec_picture->mv  [LIST_1][j4+jj][i4+ii][k] = vec;
                  currMB->mvd      [LIST_1][j+jj] [i+ii] [k] = curr_mvd;
                }
              }
            }
          }
        }
      }
    }
  }
  // record reference picture Ids for deblocking decisions

  for(j4=img->block_y;j4<(img->block_y+4);j4++)
  {
    for(i4=img->block_x;i4<(img->block_x+4);i4++)
    {
      if (dec_picture->ref_idx[LIST_0][j4][i4]>=0)
         dec_picture->ref_pic_id[LIST_0][j4][i4] = dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][(short)dec_picture->ref_idx[LIST_0][j4][i4]];
      else
         dec_picture->ref_pic_id[LIST_0][j4][i4] = INT64_MIN;
      
      if (dec_picture->ref_idx[LIST_1][j4][i4]>=0)
         dec_picture->ref_pic_id[LIST_1][j4][i4] = dec_picture->ref_pic_num[img->current_slice_nr][LIST_1 + list_offset][(short)dec_picture->ref_idx[LIST_1][j4][i4]];
      else
         dec_picture->ref_pic_id[LIST_1][j4][i4] = INT64_MIN;
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    Get the Prediction from the Neighboring Blocks for Number of Nonzero Coefficients
 *
 *    Luma Blocks
 ************************************************************************
 */
int predict_nnz(Macroblock *currMB, int block_type, struct img_par *img, int i,int j)
{
  PixelPos pix;

  int pred_nnz = 0;
  int cnt      = 0;

  // left block
  getLuma4x4Neighbour(currMB, i - 1, j, &pix);

  if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && (img->currentSlice->dp_mode==PAR_DP_3))
  {
    pix.available &= img->intra_block[pix.mb_addr];
    if (!pix.available)
      cnt++;
  }

  if (pix.available)
  { 
    switch (block_type)
    {
    case LUMA:
      pred_nnz = img->nz_coeff [pix.mb_addr ][pix.x][pix.y];
      cnt++;
      break;
    case CB:
      pred_nnz = img->nz_coeff [pix.mb_addr ][pix.x][4+pix.y];
      cnt++;
      break;
    case CR:
      pred_nnz = img->nz_coeff [pix.mb_addr ][pix.x][8+pix.y];
      cnt++;
      break;
    default:
      error("writeCoeff4x4_CAVLC: Invalid block type", 600);
      break;
    }
  }

  // top block
  getLuma4x4Neighbour(currMB, i, j - 1, &pix);

  if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && (img->currentSlice->dp_mode==PAR_DP_3))
  {
    pix.available &= img->intra_block[pix.mb_addr];
    if (!pix.available)
      cnt++;
  }

  if (pix.available)
  {
    switch (block_type)
    {
    case LUMA:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][pix.y];
      cnt++;
      break;
    case CB:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][4+pix.y];
      cnt++;
      break;
    case CR:
      pred_nnz += img->nz_coeff [pix.mb_addr ][pix.x][8+pix.y];
      cnt++;
      break;
    default:
      error("writeCoeff4x4_CAVLC: Invalid block type", 600);
      break;
    }
  }

  if (cnt==2)
  {
    pred_nnz++;
    pred_nnz>>=1;
  }

  return pred_nnz;
}


/*!
 ************************************************************************
 * \brief
 *    Get the Prediction from the Neighboring Blocks for Number of Nonzero Coefficients
 *
 *    Chroma Blocks
 ************************************************************************
 */
int predict_nnz_chroma(Macroblock *currMB, struct img_par *img, int i,int j)
{
  PixelPos pix;

  int pred_nnz = 0;
  int cnt      =0;

  if (dec_picture->chroma_format_idc != YUV444)
  {
    //YUV420 and YUV422
    // left block
    getChroma4x4Neighbour(currMB, ((i&0x01)<<2) - 1, ((j-4)<<2), &pix);

    if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && (img->currentSlice->dp_mode==PAR_DP_3))
    {
      pix.available &= img->intra_block[pix.mb_addr];
      if (!pix.available)
        cnt++;
    }

    if (pix.available)
    {
      pred_nnz = img->nz_coeff [pix.mb_addr ][2 * (i>>1) + pix.x][4 + pix.y];
      cnt++;
    }

    // top block
    getChroma4x4Neighbour(currMB, ((i&0x01)<<2), ((j-4)<<2) - 1, &pix);

    if (IS_INTRA(currMB) && pix.available && active_pps->constrained_intra_pred_flag && (img->currentSlice->dp_mode==PAR_DP_3))
    {
      pix.available &= img->intra_block[pix.mb_addr];
      if (!pix.available)
        cnt++;
    }

    if (pix.available)
    {
      pred_nnz += img->nz_coeff [pix.mb_addr ][2 * (i>>1) + pix.x][4 + pix.y];
      cnt++;
    }
  }

  if (cnt==2)
  {
    pred_nnz++;
    pred_nnz>>=1;
  }

  return pred_nnz;
}


/*!
 ************************************************************************
 * \brief
 *    Reads coeff of an 4x4 block (CAVLC)
 *
 * \author
 *    Karl Lillevold <karll@real.com>
 *    contributions by James Au <james@ubvideo.com>
 ************************************************************************
 */
void readCoeff4x4_CAVLC (Macroblock *currMB, struct img_par *img,struct inp_par *inp,
                         int block_type,
                         int i, int j, int levarr[16], int runarr[16],
                         int *number_coefficients)
{
  int mb_nr = img->current_mb_nr;
  SyntaxElement currSE;
  Slice *currSlice = img->currentSlice;
  DataPartition *dP;
  int *partMap = assignSE2partition[currSlice->dp_mode];
  Bitstream *currStream;

  int k, code, vlcnum;
  int numcoeff, numtrailingones, numcoeff_vlc;
  int level_two_or_higher;
  int numones, totzeros, level, cdc=0, cac=0;
  int zerosleft, ntr, dptype = 0;
  int max_coeff_num = 0, nnz;
  char type[15];
  static int incVlc[] = {0,3,6,12,24,48,32768};    // maximum vlc = 6

  numcoeff = 0;

  switch (block_type)
  {
  case LUMA:
    max_coeff_num = 16;
#if TRACE
    sprintf(type, "%s", "Luma");
#endif
    dptype = IS_INTRA (currMB) ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER;
    img->nz_coeff[mb_nr][i][j] = 0; 
    break;
  case LUMA_INTRA16x16DC:
    max_coeff_num = 16;
#if TRACE
    sprintf(type, "%s", "Lum16DC");
#endif
    dptype = SE_LUM_DC_INTRA;
    img->nz_coeff[mb_nr][i][j] = 0; 
    break;
  case LUMA_INTRA16x16AC:
    max_coeff_num = 15;
#if TRACE
    sprintf(type, "%s", "Lum16AC");
#endif
    dptype = SE_LUM_AC_INTRA;
    img->nz_coeff[mb_nr][i][j] = 0; 
    break;
  case CB:
    max_coeff_num = 16;
#if TRACE
    sprintf(type, "%s", "Luma_add1");
#endif
    dptype = (IS_INTRA (currMB)) ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER;
    img->nz_coeff[mb_nr][i][4+j] = 0; 
    break;
  case CB_INTRA16x16DC:
    max_coeff_num = 16;
#if TRACE
    sprintf(type, "%s", "Luma_add1_16DC");
#endif
    dptype = SE_LUM_DC_INTRA;
    img->nz_coeff[mb_nr][i][j+4] = 0; 
    break;
  case CB_INTRA16x16AC:
    max_coeff_num = 15;
#if TRACE
    sprintf(type, "%s", "Luma_add1_16AC");
#endif
    dptype = SE_LUM_AC_INTRA;
    img->nz_coeff[mb_nr][i][4+j] = 0; 
    break;
  case CR:
    max_coeff_num = 16;
#if TRACE
    sprintf(type, "%s", "Luma_add2");
#endif
    dptype = (IS_INTRA (currMB)) ? SE_LUM_AC_INTRA : SE_LUM_AC_INTER;
    img->nz_coeff[mb_nr][i][8+j] = 0; 
    break;
  case CR_INTRA16x16DC:
    max_coeff_num = 16;
#if TRACE
    sprintf(type, "%s", "Luma_add2_16DC");
#endif
    dptype = SE_LUM_DC_INTRA;
    img->nz_coeff[mb_nr][i][8+j] = 0; 
    break;
  case CR_INTRA16x16AC:
    max_coeff_num = 15;
#if TRACE
    sprintf(type, "%s", "LumA_add2_16AC");
#endif
    dptype = SE_LUM_AC_INTRA;
    img->nz_coeff[mb_nr][i][8+j] = 0; 
    break;        
  case CHROMA_DC:
    max_coeff_num = img->num_cdc_coeff;
    cdc = 1;
#if TRACE
    sprintf(type, "%s", "ChrDC");
#endif
    dptype = IS_INTRA (currMB) ? SE_CHR_DC_INTRA : SE_CHR_DC_INTER;
    img->nz_coeff[mb_nr][i][j] = 0; 
    break;
  case CHROMA_AC:
    max_coeff_num = 15;
    cac = 1;
#if TRACE
    sprintf(type, "%s", "ChrAC");
#endif
    dptype = IS_INTRA (currMB) ? SE_CHR_AC_INTRA : SE_CHR_AC_INTER;
    img->nz_coeff[mb_nr][i][j] = 0; 
    break;
  default:
    error ("readCoeff4x4_CAVLC: invalid block type", 600);
    img->nz_coeff[mb_nr][i][j] = 0; 
    break;
  }
  
  currSE.type = dptype;
  dP = &(currSlice->partArr[partMap[dptype]]);
  currStream = dP->bitstream;
  
  
  if (!cdc)
  {
    
    // luma or chroma AC    
    if(block_type==LUMA || block_type==LUMA_INTRA16x16DC || block_type==LUMA_INTRA16x16AC ||block_type==CHROMA_AC)
    {
      nnz = (!cac) ? predict_nnz(currMB, LUMA, img, i<<2, j<<2) : predict_nnz_chroma(currMB, img, i, j);
    }
    else if (block_type==CB || block_type==CB_INTRA16x16DC 
      || block_type==CB_INTRA16x16AC)
    {   
      nnz = predict_nnz(currMB, CB, img, i<<2, j<<2);
    }
    else
    { 
      nnz = predict_nnz(currMB, CR, img, i<<2, j<<2);
    }
    
    if (nnz < 2)
    {
      numcoeff_vlc = 0;
    }
    else if (nnz < 4)
    {
      numcoeff_vlc = 1;
    }
    else if (nnz < 8)
    {
      numcoeff_vlc = 2;
    }
    else //
    {
      numcoeff_vlc = 3;
    }
    
    currSE.value1 = numcoeff_vlc;
    
    readSyntaxElement_NumCoeffTrailingOnes(&currSE, currStream, type);
    
    numcoeff =  currSE.value1;
    numtrailingones =  currSE.value2;
    
    if(block_type==LUMA || block_type==LUMA_INTRA16x16DC || block_type==LUMA_INTRA16x16AC
      ||block_type==CHROMA_AC)
      img->nz_coeff[mb_nr][i][j] = numcoeff;
    else if (block_type==CB || block_type==CB_INTRA16x16DC 
      || block_type==CB_INTRA16x16AC)
      img->nz_coeff[mb_nr][i][4+j] = numcoeff;
    else
      img->nz_coeff[mb_nr][i][8+j] = numcoeff;
        
  }
  else
  {
    // chroma DC
    readSyntaxElement_NumCoeffTrailingOnesChromaDC(&currSE, currStream);
    
    numcoeff =  currSE.value1;
    numtrailingones =  currSE.value2;
  }
  
  memset(levarr, 0, max_coeff_num * sizeof(int));
  memset(runarr, 0, max_coeff_num * sizeof(int));
  
  numones = numtrailingones;
  *number_coefficients = numcoeff;
  
  if (numcoeff)
  {
    if (numtrailingones)
    {
      
      currSE.len = numtrailingones;
      
#if TRACE
      snprintf(currSE.tracestring,
        TRACESTRING_SIZE, "%s trailing ones sign (%d,%d)", type, i, j);
#endif
      
      readSyntaxElement_FLC (&currSE, currStream);
      
      code = currSE.inf;
      ntr = numtrailingones;
      for (k = numcoeff - 1; k > numcoeff - 1 - numtrailingones; k--)
      {
        ntr --;
        levarr[k] = (code>>ntr)&1 ? -1 : 1;
      }
    }
    
    // decode levels
    level_two_or_higher = (numcoeff > 3 && numtrailingones == 3)? 0 : 1;
    vlcnum = (numcoeff > 10 && numtrailingones < 3) ? 1 : 0;
    
    for (k = numcoeff - 1 - numtrailingones; k >= 0; k--)
    {
      
#if TRACE
      snprintf(currSE.tracestring,
        TRACESTRING_SIZE, "%s lev (%d,%d) k=%d vlc=%d ", type,
        i, j, k, vlcnum);
#endif
      
      if (vlcnum == 0)
        readSyntaxElement_Level_VLC0(&currSE, currStream);
      else
        readSyntaxElement_Level_VLCN(&currSE, vlcnum, currStream);
      
      if (level_two_or_higher)
      {
        currSE.inf += (currSE.inf > 0) ? 1 : -1;
        level_two_or_higher = 0;
      }
      
      level = levarr[k] = currSE.inf;
      if (iabs(level) == 1)
        numones ++;
      
      // update VLC table
      if (iabs(level) > incVlc[vlcnum])
        vlcnum++;
      
      if (k == numcoeff - 1 - numtrailingones && iabs(level)>3)
        vlcnum = 2;
      
    }
    
    if (numcoeff < max_coeff_num)
    {
      // decode total run
      vlcnum = numcoeff - 1;
      currSE.value1 = vlcnum;
      
#if TRACE
      snprintf(currSE.tracestring,
        TRACESTRING_SIZE, "%s totalrun (%d,%d) vlc=%d ", type, i,j, vlcnum);
#endif
      if (cdc)
        readSyntaxElement_TotalZerosChromaDC(&currSE, currStream);
      else
        readSyntaxElement_TotalZeros(&currSE, currStream);
      
      totzeros = currSE.value1;
    }
    else
    {
      totzeros = 0;
    }
    
    // decode run before each coefficient
    zerosleft = totzeros;
    i = numcoeff - 1;
    if (zerosleft > 0 && i > 0)
    {
      do
      {
        // select VLC for runbefore
        vlcnum = imin(zerosleft - 1, RUNBEFORE_NUM_M1);

        currSE.value1 = vlcnum;
#if TRACE
        snprintf(currSE.tracestring,
          TRACESTRING_SIZE, "%s run (%d,%d) k=%d vlc=%d ",
          type, i, j, i, vlcnum);
#endif
        
        readSyntaxElement_Run(&currSE, currStream);
        runarr[i] = currSE.value1;
        
        zerosleft -= runarr[i];
        i --;
      } while (zerosleft != 0 && i != 0);
    }
    runarr[i] = zerosleft;
    
  } // if numcoeff
}



/*!
 ************************************************************************
 * \brief
 *    Calculate the quantisation and inverse quantisation parameters
 *
 ************************************************************************
 */
void CalculateQuant8Param()
{
  int i, j, k, temp;

  for(k=0; k<6; k++)
    for(j=0; j<8; j++)
    {
      for(i=0; i<8; i++)
      {
        temp = (i<<3)+j;
        InvLevelScale8x8Luma_Intra[k][i][j] = dequant_coef8[k][j][i]*qmatrix[6][temp];
        InvLevelScale8x8Luma_Inter[k][i][j] = dequant_coef8[k][j][i]*qmatrix[7][temp];
      }
    }

    if( active_sps->chroma_format_idc == 3 )  // 4:4:4
    {
      for(k=0; k<6; k++)
        for(j=0; j<8; j++)
        {
          for(i=0; i<8; i++)
          {
            temp = (i<<3)+j;
            InvLevelScale8x8Chroma_Intra[0][k][i][j] = dequant_coef8[k][j][i]*qmatrix[8][temp];
            InvLevelScale8x8Chroma_Inter[0][k][i][j] = dequant_coef8[k][j][i]*qmatrix[9][temp];
            InvLevelScale8x8Chroma_Intra[1][k][i][j] = dequant_coef8[k][j][i]*qmatrix[10][temp];
            InvLevelScale8x8Chroma_Inter[1][k][i][j] = dequant_coef8[k][j][i]*qmatrix[11][temp];
          }
        }
    }
}

/*!
************************************************************************
* \brief
*    Get coefficients (run/level) of one 8x8 block
*    from the NAL (CABAC Mode)
************************************************************************
*/
void readLumaCoeff8x8_CABAC (Macroblock *currMB, ColorPlane pl, struct img_par *img,struct inp_par *inp, int b8)
{
  int i,j,k;
  int level = 1;
  int cbp = currMB->cbp;
  SyntaxElement currSE;
  Slice *currSlice = img->currentSlice;
  DataPartition *dP;
  int *partMap = assignSE2partition[currSlice->dp_mode];
  int start_scan = 0; // take all coeffs
  int coef_ctr = start_scan - 1;// i0, j0;
  int boff_x, boff_y;

  int run, len;

  int qp, qp_c, qp_per, qp_rem; 
  int uv = pl-1; 

  Boolean lossless_qpprime = (Boolean) ((img->qp + img->bitdepth_luma_qp_scale)==0 && img->lossless_qpprime_flag==1);
  int (*InvLevelScale8x8)[8] = NULL;
  // select scan type
  const byte (*pos_scan8x8)[2] = ((img->structure == FRAME) && (!currMB->mb_field)) ? SNGL_SCAN8x8 : FIELD_SCAN8x8;

  if (pl)
  {
    qp = img->qp + dec_picture->chroma_qp_offset[uv];
    qp = iClip3(-(img->bitdepth_chroma_qp_scale), 51, qp); 
    qp_c  = (qp < 0)? qp : QP_SCALE_CR[qp-MIN_QP];
    qp_per = (qp_c + img->bitdepth_chroma_qp_scale)/6;
    qp_rem = (qp_c + img->bitdepth_chroma_qp_scale)%6;
  }
  else
  {
    qp_per    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)/6;
    qp_rem    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)%6;
  }

  if( IS_INDEPENDENT(img) )
  {
    if( img->colour_plane_id == 0 )
      InvLevelScale8x8 = IS_INTRA(currMB)? InvLevelScale8x8Luma_Intra[qp_rem] : InvLevelScale8x8Luma_Inter[qp_rem];
    else if( img->colour_plane_id == 1 )
      InvLevelScale8x8 = IS_INTRA(currMB)? InvLevelScale8x8Chroma_Intra[0][qp_rem] : InvLevelScale8x8Chroma_Inter[0][qp_rem];
    else if( img->colour_plane_id == 2 )
      InvLevelScale8x8 = IS_INTRA(currMB)? InvLevelScale8x8Chroma_Intra[1][qp_rem] : InvLevelScale8x8Chroma_Inter[1][qp_rem];
  }
  else
    InvLevelScale8x8 = IS_INTRA(currMB)? InvLevelScale8x8Luma_Intra[qp_rem] : InvLevelScale8x8Luma_Inter[qp_rem];

  img->is_intra_block = IS_INTRA(currMB);

  if (cbp & (1<<b8))  // are there any coeff in current block at all
  {
    // === set offset in current macroblock ===
    boff_x = (b8&0x01) << 3;
    boff_y = (b8 >> 1) << 3;

    img->subblock_x = boff_x >> 2; // position for coeff_count ctx
    img->subblock_y = boff_y >> 2; // position for coeff_count ctx

    if (pl==PLANE_Y)  
      currSE.context = LUMA_8x8;
    else if (pl==PLANE_U)
      currSE.context = CB_8x8;
    else
      currSE.context = CR_8x8;  

    if( IS_INDEPENDENT(img) )
      currSE.context = LUMA_8x8;

    if(!lossless_qpprime)
    {
      for(k=start_scan;(k < 65) && (level != 0);k++)
      {
        //============ read =============
        /*
        * make distinction between INTRA and INTER coded
        * luminance coefficients
        */
         
        currSE.type    = ((img->is_intra_block == 1)
          ? (k==0 ? SE_LUM_DC_INTRA : SE_LUM_AC_INTRA) 
          : (k==0 ? SE_LUM_DC_INTER : SE_LUM_AC_INTER));

#if TRACE
        if (pl==PLANE_Y)
          sprintf(currSE.tracestring, "Luma8x8 sng ");
        else if (pl==PLANE_U)
          sprintf(currSE.tracestring, "Cb  8x8 sng "); 
        else 
          sprintf(currSE.tracestring, "Cr  8x8 sng "); 
#endif

        dP = &(currSlice->partArr[partMap[currSE.type]]);
        currSE.reading = readRunLevel_CABAC;

        dP->readSyntaxElement(&currSE,img,dP);
        level = currSE.value1;
        run   = currSE.value2;
        len   = currSE.len;

        //============ decode =============
        if (level != 0)    /* leave if len=1 */
        {
          coef_ctr += run + 1;

          i=pos_scan8x8[coef_ctr][0];
          j=pos_scan8x8[coef_ctr][1];

          if (pl==PLANE_Y) 
          {
            currMB->cbp_blk |= 51 << (4 * b8 - 2 * (b8 & 0x01)); // corresponds to 110011, as if all four 4x4 blocks contain coeff, shifted to block position            
          }
          else 
          {
            currMB->cbp_blk_CbCr[uv] |= 51 << (4 * b8 - 2 * (b8 & 0x01));             
          }

          img->m7[pl][boff_y + j][boff_x + i] = rshift_rnd_sf((level * InvLevelScale8x8[j][i]) << qp_per, 6); // dequantization
        }
      }
    }
    else
    {
      for(k=start_scan;(k < 65) && (level != 0);k++)
      {
        //============ read =============
        /*
        * make distinction between INTRA and INTER coded
        * luminance coefficients
        */

        currSE.type    = ((img->is_intra_block == 1)
          ? (k==0 ? SE_LUM_DC_INTRA : SE_LUM_AC_INTRA) 
          : (k==0 ? SE_LUM_DC_INTER : SE_LUM_AC_INTER));

#if TRACE
        if (pl==PLANE_Y)
          sprintf(currSE.tracestring, "Luma8x8 sng ");
        else if (pl==PLANE_U)
          sprintf(currSE.tracestring, "Cb  8x8 sng "); 
        else 
          sprintf(currSE.tracestring, "Cr  8x8 sng "); 
#endif

        dP = &(currSlice->partArr[partMap[currSE.type]]);
        currSE.reading = readRunLevel_CABAC;

        dP->readSyntaxElement(&currSE,img,dP);
        level = currSE.value1;
        run   = currSE.value2;
        len   = currSE.len;

        //============ decode =============
        if (level != 0)    /* leave if len=1 */
        {
          coef_ctr += run + 1;

          i=pos_scan8x8[coef_ctr][0];
          j=pos_scan8x8[coef_ctr][1];

          if (pl==PLANE_Y)
          {
            currMB->cbp_blk |= 51 << (4 * b8 - 2 * (b8 & 0x01)); // corresponds to 110011, as if all four 4x4 blocks contain coeff, shifted to block position            
          }
          else
          {
            currMB->cbp_blk_CbCr[uv] |= 51 << (4 * b8 - 2 * (b8 & 0x01)); // corresponds to 110011, as if all four 4x4 blocks contain coeff, shifted to block position
          }

          img->m7[pl][boff_y + j][boff_x + i] = level;
        }
      }
    }
  }
}

/*!
************************************************************************
* \brief
*    Data partitioning: Check if neighboring macroblock is needed for 
*    CAVLC context decoding, and disable current MB if data partition
*    is missing.
************************************************************************
*/
void check_dp_neighbors (Macroblock *currMB)
{
  PixelPos up, left;

  getNeighbour(currMB, -1,  0, 1, &left);
  getNeighbour(currMB,  0, -1, 1, &up);

  if (IS_INTER (currMB) || (IS_INTRA (currMB) && !(active_pps->constrained_intra_pred_flag)) )
  {
    if (left.available)
    {
      currMB->dpl_flag |= img->mb_data[left.mb_addr].dpl_flag;
    }
    if (up.available)
    {
      currMB->dpl_flag |= img->mb_data[up.mb_addr].dpl_flag;
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Get coded block pattern and coefficients (run/level)
 *    from the NAL
 ************************************************************************
 */
void readCBPandCoeffsFromNAL(Macroblock *currMB, struct img_par *img,struct inp_par *inp)
{
  int i,j,k;
  int level;
  int mb_nr = img->current_mb_nr;
  int ii,jj;
  int cbp;
  SyntaxElement currSE;
  Slice *currSlice = img->currentSlice;
  DataPartition *dP;
  int *partMap = assignSE2partition[currSlice->dp_mode];
  int coef_ctr, i0, j0, b8;
  int ll;
  int block_x,block_y;
  int start_scan;
  int run, len;
  int levarr[16], runarr[16], numcoeff;
  
  int qp_const;
  int qp_per    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)/6;
  int qp_rem    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)%6;
  int smb       = ((img->type==SP_SLICE) && IS_INTER (currMB)) || (img->type == SI_SLICE && currMB->mb_type == SI4MB);
  
  int uv; 
    int qp_const_uv[2]; 
  int qp_per_uv[2];
  int qp_rem_uv[2];
  
  int intra     = IS_INTRA (currMB);
  int temp[4];
  
  int b4;
  int yuv = dec_picture->chroma_format_idc - 1;
  int m5[4];
  int m6[4];
  
  int need_transform_size_flag;
  Boolean lossless_qpprime = (Boolean) ((img->qp + img->bitdepth_luma_qp_scale)==0 && img->lossless_qpprime_flag==1);
  
  int (*InvLevelScale4x4)[4] = NULL;
  int (*InvLevelScale8x8)[8] = NULL;
  // select scan type
  const byte (*pos_scan8x8)[2] = ((img->structure == FRAME) && (!currMB->mb_field)) ? SNGL_SCAN8x8 : FIELD_SCAN8x8;
  const byte (*pos_scan4x4)[2] = ((img->structure == FRAME) && (!currMB->mb_field)) ? SNGL_SCAN : FIELD_SCAN;

  if(img->type==SP_SLICE  && currMB->mb_type!=I16MB )
    smb=1;
  
  // QPI
  //init constants for every chroma qp offset
  if (dec_picture->chroma_format_idc != YUV400)
  {
    for (i=0; i<2; i++)
    {
      qp_per_uv[i] = (currMB->qpc[i] + img->bitdepth_chroma_qp_scale)/6;
      qp_rem_uv[i] = (currMB->qpc[i] + img->bitdepth_chroma_qp_scale)%6;
    }
  }
  
  // read CBP if not new intra mode
  if (!IS_NEWINTRA (currMB))
  {
    //=====   C B P   =====
    //---------------------
    currSE.type = (IS_OLDINTRA (currMB) || currMB->mb_type == SI4MB || currMB->mb_type == I8MB) 
      ? SE_CBP_INTRA
      : SE_CBP_INTER;
    
    dP = &(currSlice->partArr[partMap[currSE.type]]);

    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
    {
      currSE.mapping = (IS_OLDINTRA (currMB) || currMB->mb_type == SI4MB || currMB->mb_type == I8MB)
        ? linfo_cbp_intra
        : linfo_cbp_inter;
    }
    else
    {
      currSE.reading = readCBP_CABAC;
    }
    
    TRACE_STRING("coded_block_pattern");
    dP->readSyntaxElement(&currSE,img,dP);
    currMB->cbp = cbp = currSE.value1;
    
    
    //============= Transform size flag for INTER MBs =============
    //-------------------------------------------------------------
    need_transform_size_flag = (((currMB->mb_type >= 1 && currMB->mb_type <= 3)||
                                (IS_DIRECT(currMB) && active_sps->direct_8x8_inference_flag) ||
                                (currMB->NoMbPartLessThan8x8Flag))
                                && currMB->mb_type != I8MB && currMB->mb_type != I4MB
                                && (currMB->cbp&15)
                                && img->Transform8x8Mode);

    if (need_transform_size_flag)
    {
      currSE.type   =  SE_HEADER;
      dP = &(currSlice->partArr[partMap[SE_HEADER]]);
      currSE.reading = readMB_transform_size_flag_CABAC;
      TRACE_STRING("transform_size_8x8_flag");

      // read UVLC transform_size_8x8_flag
      if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
      {
        currSE.len = 1;
        readSyntaxElement_FLC(&currSE, dP->bitstream);
      } 
      else
      {
        dP->readSyntaxElement(&currSE,img,dP);
      }
      currMB->luma_transform_size_8x8_flag = currSE.value1;
    }

    //=====   DQUANT   =====
    //----------------------
    // Delta quant only if nonzero coeffs
    if (cbp !=0)
    {
      currSE.type = (IS_INTER (currMB)) ? SE_DELTA_QUANT_INTER : SE_DELTA_QUANT_INTRA;

      dP = &(currSlice->partArr[partMap[currSE.type]]);

      if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
      {
        currSE.mapping = linfo_se;
      }
      else
        currSE.reading= readDquant_CABAC;

      TRACE_STRING("mb_qp_delta");

      dP->readSyntaxElement(&currSE,img,dP);
      currMB->delta_quant = currSE.value1;
      if ((currMB->delta_quant < -(26 + img->bitdepth_luma_qp_scale/2)) || (currMB->delta_quant > (25 + img->bitdepth_luma_qp_scale/2)))
        error ("mb_qp_delta is out of range", 500);

      img->qp = ((img->qp + currMB->delta_quant + 52 + 2*img->bitdepth_luma_qp_scale)%(52+img->bitdepth_luma_qp_scale)) -
        img->bitdepth_luma_qp_scale;
      
      if (currSlice->dp_mode)
      {
        if (IS_INTER (currMB) && currSlice->dpC_NotPresent ) 
          currMB->dpl_flag = 1;

        if( IS_INTRA (currMB) && currSlice->dpB_NotPresent )
        {
          currMB->ei_flag = 1;
          currMB->dpl_flag = 1;
        }

        // check for prediction from neighbours
        check_dp_neighbors (currMB);
        if (currMB->dpl_flag)
        {
          cbp = 0; 
          currMB->cbp = cbp;
        }
      }
    }
  }
  else
  {
    cbp = currMB->cbp;
  }
  
  memset(&img->cof[0][0][0], 0, MB_PIXELS * sizeof(int)); // reset luma coeffs   
    
  if (IS_NEWINTRA (currMB)) // read DC coeffs for new intra modes
  {
    currSE.type = SE_DELTA_QUANT_INTRA;
    
    dP = &(currSlice->partArr[partMap[currSE.type]]);
    
    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
    {
      currSE.mapping = linfo_se;
    }
    else
    {
      currSE.reading= readDquant_CABAC;
    }
#if TRACE
    snprintf(currSE.tracestring, TRACESTRING_SIZE, "Delta quant ");
#endif
    dP->readSyntaxElement(&currSE,img,dP);
    currMB->delta_quant = currSE.value1;
    if ((currMB->delta_quant < -(26 + img->bitdepth_luma_qp_scale/2)) || (currMB->delta_quant > (25 + img->bitdepth_luma_qp_scale/2)))
      error ("mb_qp_delta is out of range", 500);
    
    img->qp= ((img->qp + currMB->delta_quant + 52 + 2*img->bitdepth_luma_qp_scale)%(52+img->bitdepth_luma_qp_scale)) -
      img->bitdepth_luma_qp_scale;
    
    for (j=0;j<BLOCK_SIZE;j++)
      memset(&img->ipredmode[img->block_y+j][img->block_x], DC_PRED,  BLOCK_SIZE * sizeof(byte));
        
    if (currSlice->dp_mode)
    {  
      if (currSlice->dpB_NotPresent)
      {
        currMB->ei_flag  = 1;
        currMB->dpl_flag = 1;
      }
      check_dp_neighbors (currMB);
      if (currMB->dpl_flag)
      {
        cbp =0; 
        currMB->cbp      = cbp;
      }
    }
    if (!currMB->dpl_flag)
    {
      if (active_pps->entropy_coding_mode_flag == UVLC)
      {
        readCoeff4x4_CAVLC(currMB, img, inp, LUMA_INTRA16x16DC, 0, 0,
          levarr, runarr, &numcoeff);

        coef_ctr=-1;
        level = 1;                            // just to get inside the loop
        for(k = 0; k < numcoeff; k++)
        {
          if (levarr[k] != 0)                     // leave if len=1
          {
            coef_ctr += runarr[k] + 1;

            i0=pos_scan4x4[coef_ctr][0];
            j0=pos_scan4x4[coef_ctr][1];

            img->cof[0][j0<<2][i0<<2]=levarr[k];// add new intra DC coeff
          }
        }
      }
      else
      {
        currSE.type = SE_LUM_DC_INTRA;
        dP = &(currSlice->partArr[partMap[currSE.type]]);

        currSE.context      = LUMA_16DC;
        currSE.type         = SE_LUM_DC_INTRA;
        img->is_intra_block = 1;

        if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
        {
          currSE.mapping = linfo_levrun_inter;
        }
        else
        {
          currSE.reading = readRunLevel_CABAC;
        }

        coef_ctr = -1;
        level = 1;                            // just to get inside the loop

        for(k=0;(k<17) && (level!=0);k++)
        {
#if TRACE
          snprintf(currSE.tracestring, TRACESTRING_SIZE, "DC luma 16x16 ");
#endif
          dP->readSyntaxElement(&currSE,img,dP);
          level = currSE.value1;
          run   = currSE.value2;
          len   = currSE.len;

          if (level != 0)                     // leave if len=1
          {
            coef_ctr=coef_ctr+run+1;

            i0=pos_scan4x4[coef_ctr][0];
            j0=pos_scan4x4[coef_ctr][1];

            img->cof[0][j0<<2][i0<<2]=level;// add new intra DC coeff
          }
        }
      }
      if(!lossless_qpprime)
        itrans_2(PLANE_Y, img);// transform new intra DC
    }
  }
  currMB->qp = img->qp;
  set_chroma_qp(currMB);
  
  qp_per    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)/6;
  qp_rem    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)%6;
  qp_const  = 1<<(3-qp_per);
  
  if( IS_INDEPENDENT(img) )
  {
    if( img->colour_plane_id == 0 )
    {
      InvLevelScale4x4 = intra? InvLevelScale4x4Luma_Intra[qp_rem] : InvLevelScale4x4Luma_Inter[qp_rem];
      InvLevelScale8x8 = intra? InvLevelScale8x8Luma_Intra[qp_rem] : InvLevelScale8x8Luma_Inter[qp_rem];
    }
    else if( img->colour_plane_id == 1 )
    {
      InvLevelScale4x4 = intra? InvLevelScale4x4Chroma_Intra[0][qp_rem] : InvLevelScale4x4Chroma_Inter[0][qp_rem];
      InvLevelScale8x8 = intra? InvLevelScale8x8Chroma_Intra[0][qp_rem] : InvLevelScale8x8Chroma_Inter[0][qp_rem];
    }
    else if( img->colour_plane_id == 2 )
    {
      InvLevelScale4x4 = intra? InvLevelScale4x4Chroma_Intra[1][qp_rem] : InvLevelScale4x4Chroma_Inter[1][qp_rem];
      InvLevelScale8x8 = intra? InvLevelScale8x8Chroma_Intra[1][qp_rem] : InvLevelScale8x8Chroma_Inter[1][qp_rem];
    }
  }
  else
  {
    InvLevelScale4x4 = intra? InvLevelScale4x4Luma_Intra[qp_rem] : InvLevelScale4x4Luma_Inter[qp_rem];
    InvLevelScale8x8 = intra? InvLevelScale8x8Luma_Intra[qp_rem] : InvLevelScale8x8Luma_Inter[qp_rem];
  }
    
  //init constants for every chroma qp offset
  if (dec_picture->chroma_format_idc != YUV400)
  {
    for(i=0; i < 2; i++)
    {
      qp_per_uv[i] = (currMB->qpc[i] + img->bitdepth_chroma_qp_scale)/6;
      qp_rem_uv[i] = (currMB->qpc[i] + img->bitdepth_chroma_qp_scale)%6;
      qp_const_uv[i] = 1<<(3-qp_per_uv[i]);
    }
  }
    
  
  // luma coefficients
  for (block_y=0; block_y < 4; block_y += 2) /* all modes */
  {
    for (block_x=0; block_x < 4; block_x += 2)
    {      
      b8 = 2*(block_y>>1) + (block_x>>1);
      if (active_pps->entropy_coding_mode_flag == UVLC)
      {
        for (j=block_y; j < block_y+2; j++)
        {
          for (i=block_x; i < block_x+2; i++)
          {
            ii = block_x >> 1;
            jj = block_y >> 1;
            b8 = 2 * jj + ii;
            
            if (cbp & (1<<b8))  /* are there any coeff in current block at all */
            {
              readCoeff4x4_CAVLC(currMB, img, inp, (IS_NEWINTRA(currMB) ? LUMA_INTRA16x16AC : LUMA), i, j, levarr, runarr, &numcoeff);
              
              start_scan = IS_NEWINTRA(currMB) ? 1 : 0;
              coef_ctr = start_scan - 1;
              
              if(!lossless_qpprime)
              {
                if (!currMB->luma_transform_size_8x8_flag) // 4x4 transform
                {
                  for (k = 0; k < numcoeff; k++)
                  {
                    if (levarr[k] != 0)
                    {
                      coef_ctr += runarr[k]+1;
                      
                      i0 = pos_scan4x4[coef_ctr][0];
                      j0 = pos_scan4x4[coef_ctr][1];
                      
                      // inverse quant for 4x4 transform only
                      currMB->cbp_blk      |= (int64) 1 << ((j<<2) + i);
                      img->cof[0][(j<<2) + j0][(i<<2) + i0]= rshift_rnd_sf((levarr[k]*InvLevelScale4x4[j0][i0])<<qp_per, 4);
                    }
                  }
                }
                else // 8x8 transform
                {
                  int b4, iz, jz;
                  for (k = 0; k < numcoeff; k++)
                  {
                    if (levarr[k] != 0)
                    {
                      coef_ctr += runarr[k]+1;
                      
                      // do same as CABAC for deblocking: any coeff in the 8x8 marks all the 4x4s
                      //as containing coefficients
                      currMB->cbp_blk  |= 51 << ((block_y<<2) + block_x);
                      
                      b4 = 2*(j - block_y)+(i - block_x);
                      
                      iz = pos_scan8x8[(coef_ctr << 2) + b4][0];
                      jz = pos_scan8x8[(coef_ctr << 2) + b4][1];
                      
                      img->m7[0][block_y*4 +jz][block_x*4 +iz] = rshift_rnd_sf((levarr[k]*InvLevelScale8x8[jz][iz])<<qp_per, 6); // dequantization
                    }
                  }//else (!currMB->luma_transform_size_8x8_flag)
                }
              }
              else
              {
                if (!currMB->luma_transform_size_8x8_flag) // inverse quant for 4x4 transform
                {
                  for (k = 0; k < numcoeff; k++)
                  {
                    if (levarr[k] != 0)
                    {
                      coef_ctr += runarr[k]+1;
                      
                      i0=pos_scan4x4[coef_ctr][0];
                      j0=pos_scan4x4[coef_ctr][1];
                      
                      currMB->cbp_blk      |= (int64) 1 << ((j<<2) + i);
                      img->cof[0][(j<<2) + j0][(i<<2) + i0]= levarr[k];
                    }
                  }
                }
                else // inverse quant for 8x8 transform
                {
                  int b4, iz, jz;
                  for (k = 0; k < numcoeff; k++)
                  {
                    if (levarr[k] != 0)
                    {
                      coef_ctr += runarr[k]+1;
                      
                      // do same as CABAC for deblocking: any coeff in the 8x8 marks all the 4x4s
                      //as containing coefficients
                      currMB->cbp_blk  |= 51 << ((block_y<<2) + block_x);
                      
                      b4 = 2*(j-block_y)+(i-block_x);
                      
                      iz=pos_scan8x8[coef_ctr*4+b4][0];
                      jz=pos_scan8x8[coef_ctr*4+b4][1];
                      
                      img->m7[0][block_y*4 +jz][block_x*4 +iz] = levarr[k];
                    }
                  }
                }//else (!currMB->luma_transform_size_8x8_flag)
              }
            }
            else
            {
              img->nz_coeff[mb_nr][i][j] = 0;
            }
          }
        }
      } // VLC
      else
      {
        if(currMB->luma_transform_size_8x8_flag)
          readLumaCoeff8x8_CABAC(currMB, PLANE_Y, img, inp, b8); //======= 8x8 trannsform size & CABAC ========
        else
        {
          //======= Other Modes & CABAC ========
          //------------------------------------
          for (j=block_y; j < block_y+2; j++)
          {
            img->subblock_y = j; // position for coeff_count ctx
            for (i=block_x; i < block_x+2; i++)
            {
              start_scan = IS_NEWINTRA (currMB)? 1 : 0;
              
              img->subblock_x = i; // position for coeff_count ctx
              
              if (cbp & (1<<b8))  // are there any coeff in current block at all
              {
                coef_ctr = start_scan - 1;
                level    = 1;
                img->is_intra_block = IS_INTRA(currMB);
                
                if(!lossless_qpprime)
                {
                  for(k=start_scan;(k<17) && (level!=0);k++)
                  {
                    /*
                    * make distinction between INTRA and INTER coded
                    * luminance coefficients
                    */
                    currSE.context      = (IS_NEWINTRA(currMB) ? LUMA_16AC : LUMA_4x4);
                    currSE.type         = (img->is_intra_block 
                      ? (k==0 ? SE_LUM_DC_INTRA : SE_LUM_AC_INTRA) 
                      : (k==0 ? SE_LUM_DC_INTER : SE_LUM_AC_INTER));                                      
                    
#if TRACE
                    sprintf(currSE.tracestring, "Luma sng ");
#endif
                    dP = &(currSlice->partArr[partMap[currSE.type]]);

                    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)  
                      currSE.mapping = linfo_levrun_inter;
                    else                                                     
                      currSE.reading = readRunLevel_CABAC;

                    dP->readSyntaxElement(&currSE,img,dP);
                    level = currSE.value1;
                    run   = currSE.value2;
                    len   = currSE.len;

                    if (level != 0)    /* leave if len=1 */
                    {
                      coef_ctr += run+1;

                      i0=pos_scan4x4[coef_ctr][0];
                      j0=pos_scan4x4[coef_ctr][1];

                      currMB->cbp_blk |= (int64)1 << ((j<<2) + i) ;
                      img->cof[0][(j<<2) + j0][(i<<2) + i0]= rshift_rnd_sf((level*InvLevelScale4x4[j0][i0]) << qp_per, 4);
                    }
                  }
                }
                else
                {
                  for(k=start_scan;(k<17) && (level!=0);k++)
                  {
                    /*
                    * make distinction between INTRA and INTER coded
                    * luminance coefficients
                    */
                    currSE.context      = (IS_NEWINTRA(currMB) ? LUMA_16AC : LUMA_4x4);
                    currSE.type         = (img->is_intra_block 
                      ? (k==0 ? SE_LUM_DC_INTRA : SE_LUM_AC_INTRA) 
                      : (k==0 ? SE_LUM_DC_INTER : SE_LUM_AC_INTER));                                      

#if TRACE
                    sprintf(currSE.tracestring, "Luma sng ");
#endif
                    dP = &(currSlice->partArr[partMap[currSE.type]]);

                    if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)  
                      currSE.mapping = linfo_levrun_inter;
                    else                                                     
                      currSE.reading = readRunLevel_CABAC;

                    dP->readSyntaxElement(&currSE,img,dP);
                    level = currSE.value1;
                    run   = currSE.value2;
                    len   = currSE.len;

                    if (level != 0)    /* leave if len=1 */
                    {
                      coef_ctr += run+1;

                      i0=pos_scan4x4[coef_ctr][0];
                      j0=pos_scan4x4[coef_ctr][1];

                      currMB->cbp_blk |= (int64)1 << ((j<<2) + i) ;

                      img->cof[0][(j<<2) + j0][(i<<2) + i0] = level;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  
  if ( active_sps->chroma_format_idc==YUV444 && !IS_INDEPENDENT(img) ) 
  {
    for (uv = 0; uv < 2; uv++ )
    {
      memset(&img->cof[uv + 1][0][0], 0, MB_PIXELS * sizeof(int));

      /*----------------------16x16DC Luma_Add----------------------*/
      if (IS_NEWINTRA (currMB)) // read DC coeffs for new intra modes       
      {
        for (i=0;i<BLOCK_SIZE;i++)
          for (j=0;j<BLOCK_SIZE;j++)
            img->ipredmode[img->block_y+j][img->block_x+i]=DC_PRED;

        if (active_pps->entropy_coding_mode_flag == UVLC)
        {
          if (uv == 0)
            readCoeff4x4_CAVLC(currMB, img, inp, CB_INTRA16x16DC, 0, 0, levarr, runarr, &numcoeff);
          else
            readCoeff4x4_CAVLC(currMB, img, inp, CR_INTRA16x16DC, 0, 0, levarr, runarr, &numcoeff);

          coef_ctr=-1;
          level = 1;                            // just to get inside the loop
          for(k = 0; k < numcoeff; k++)
          {
            if (levarr[k] != 0)                     // leave if len=1
            {
              coef_ctr += runarr[k] + 1;

              i0=pos_scan4x4[coef_ctr][0];
              j0=pos_scan4x4[coef_ctr][1];
              img->cof[uv + 1][j0<<2][i0<<2]=levarr[k];// add new intra DC coeff
            } //if leavarr[k]
          } //k loop
        } //UVLC
        else // else UVLC
        {              
          currSE.type = SE_LUM_DC_INTRA;
          dP = &(currSlice->partArr[partMap[currSE.type]]);


          if (uv==0)
            currSE.context   = CB_16DC; 
          else
            currSE.context   = CR_16DC; 

		  if( IS_INDEPENDENT(img) )
            currSE.context   = LUMA_16DC; 

          currSE.type         = SE_LUM_DC_INTRA;
          img->is_intra_block = 1;

          if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
          {
            currSE.mapping = linfo_levrun_inter;
          }
          else
          {
            currSE.reading = readRunLevel_CABAC;
          }

          coef_ctr = -1;
          level = 1;                            // just to get inside the loop

          for(k=0;(k<17) && (level!=0);k++)
          {
#if TRACE
            if (uv == 0)
              snprintf(currSE.tracestring, TRACESTRING_SIZE, "DC Cb   16x16 "); 
            else
              snprintf(currSE.tracestring, TRACESTRING_SIZE, "DC Cr   16x16 ");
#endif

            dP->readSyntaxElement(&currSE,img,dP);
            level = currSE.value1;
            run   = currSE.value2;
            len   = currSE.len;

            if (level != 0)                     // leave if len=1
            {
              coef_ctr=coef_ctr+run+1;

              i0=pos_scan4x4[coef_ctr][0];
              j0=pos_scan4x4[coef_ctr][1];
              img->cof[uv + 1][j0<<2][i0<<2]=level;
            }                        
          } //k loop
        } // else UVLC

        if(!lossless_qpprime)
        {
          if (uv==0)
            itrans_2(PLANE_U, img); // transform new intra DC
          else
            itrans_2(PLANE_V, img); // transform new intra DC
        }

      } //IS_NEWINTRA

      currMB->qp = img->qp;
      set_chroma_qp(currMB);

      qp_per    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)/6;
      qp_rem    = (img->qp + img->bitdepth_luma_qp_scale - MIN_QP)%6;
      qp_const  = 1<<(3-qp_per);

      InvLevelScale4x4 = intra? InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv[uv]] : InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv[uv]];
      InvLevelScale8x8 = intra? InvLevelScale8x8Chroma_Intra[uv][qp_rem_uv[uv]] : InvLevelScale8x8Chroma_Intra[uv][qp_rem_uv[uv]];

      //init constants for every chroma qp offset
      if (dec_picture->chroma_format_idc != YUV400)
      {
        for(i=0; i < 2; i++)
        {
          qp_per_uv[i] = (currMB->qpc[i] + img->bitdepth_chroma_qp_scale)/6;
          qp_rem_uv[i] = (currMB->qpc[i] + img->bitdepth_chroma_qp_scale)%6;
          qp_const_uv[i] = 1<<(3-qp_per_uv[i]);
        } //for
      }//!= YUV400

      // luma_add coefficients
      for (block_y=0; block_y < 4; block_y += 2) /* all modes */
      {
        for (block_x=0; block_x < 4; block_x += 2)
        {
          b8 = 2*(block_y>>1) + (block_x>>1);         

          if (active_pps->entropy_coding_mode_flag == UVLC)
          {
            for (j=block_y; j < block_y+2; j++)
            {
              for (i=block_x; i < block_x+2; i++)
              {
                ii = block_x >> 1;
                jj = block_y >> 1;
                b8 = 2 * jj + ii;

                if (cbp & (1<<b8))  /* are there any coeff in current block at all */
                {
                  if (uv==0)
                    readCoeff4x4_CAVLC(currMB, img, inp, (IS_NEWINTRA(currMB) ? CB_INTRA16x16AC : CB), i, j, levarr, runarr, &numcoeff);
                  else
                    readCoeff4x4_CAVLC(currMB, img, inp, (IS_NEWINTRA(currMB) ? CR_INTRA16x16AC : CR), i, j, levarr, runarr, &numcoeff);

                  start_scan = IS_NEWINTRA(currMB) ? 1 : 0;
                  coef_ctr = start_scan - 1;

                  if(!lossless_qpprime)
                  {
                    if (!currMB->luma_transform_size_8x8_flag) // 4x4 transform
                    {
                      for (k = 0; k < numcoeff; k++)
                      {
                        if (levarr[k] != 0)
                        {
                          coef_ctr += runarr[k]+1;

                          i0 = pos_scan4x4[coef_ctr][0];
                          j0 = pos_scan4x4[coef_ctr][1];

                          // inverse quant for 4x4 transform only
                          currMB->cbp_blk_CbCr[uv]       |= (int64) 1 << ((j<<2) + i);
                          img->cof[uv + 1][(j << 2) + j0][(i << 2) + i0]= rshift_rnd_sf((levarr[k]*InvLevelScale4x4[j0][i0])<<qp_per_uv[uv],4);          
                        } //levarr[k] != 0
                      }//k loop
                    } //4x4
                    else //8x8
                    {
                      int b4, iz, jz;
                      for (k = 0; k < numcoeff; k++)
                      {
                        if (levarr[k] != 0)
                        {
                          coef_ctr += runarr[k]+1;

                          // do same as CABAC for deblocking: any coeff in the 8x8 marks all the 4x4s
                          //as containing coefficients
                          currMB->cbp_blk_CbCr[uv]  |= 51 << ((block_y<<2) + block_x);

                          b4 = 2*(j - block_y)+(i - block_x);

                          iz = pos_scan8x8[(coef_ctr << 2) + b4][0];
                          jz = pos_scan8x8[(coef_ctr << 2) + b4][1];
                          img->m7[uv+1][block_y*4 +jz][block_x*4 +iz] = rshift_rnd_sf((levarr[k]*InvLevelScale8x8[jz][iz])<<qp_per_uv[uv], 6); // dequantization 444_TEMP_NOTE 
                        } //if levarr
                      }//8x8
                    }//k loop
                  } // !lossless
                  else //lossless
                  {
                    if (!currMB->luma_transform_size_8x8_flag) // inverse quant for 4x4 transform
                    {
                      for (k = 0; k < numcoeff; k++)
                      {
                        if (levarr[k] != 0)
                        {
                          coef_ctr += runarr[k]+1;

                          i0=pos_scan4x4[coef_ctr][0];
                          j0=pos_scan4x4[coef_ctr][1];
                          currMB->cbp_blk_CbCr[uv]      |= (int64) 1 << ((j<<2) + i);
                          img->cof[uv + 1][(j << 2) + j0][(i << 2) + i0] = levarr[k];
                        } //levarr[k]
                      } //k loop
                    } //4x4
                    else //8x8
                    {
                      int b4, iz, jz;
                      for (k = 0; k < numcoeff; k++)
                      {
                        if (levarr[k] != 0)
                        {
                          coef_ctr += runarr[k]+1;

                          // do same as CABAC for deblocking: any coeff in the 8x8 marks all the 4x4s
                          //as containing coefficients
                          currMB->cbp_blk_CbCr[uv]  |= 51 << ((block_y<<2) + block_x);

                          b4 = 2*(j-block_y)+(i-block_x);

                          iz=pos_scan8x8[coef_ctr*4+b4][0];
                          jz=pos_scan8x8[coef_ctr*4+b4][1];

                          img->m7[uv+1][block_y*4 +jz][block_x*4 +iz] = levarr[k];
                        } //levarr[k]
                      } //k loop
                    } //8x8
                  }// loseless
                } //if (cbp & (1<<b8))                                
                else //!(cbp & (1<<b8))
                {
                  img->nz_coeff[mb_nr][i][(4 << uv) +j] = 0;
                } //!(cbp & (1<<b8))
              } //i=block_x
            } //j=block_y
          } //UVCL
          else // CABAC
          {            
            if(currMB->luma_transform_size_8x8_flag) 
            {
                readLumaCoeff8x8_CABAC(currMB, (ColorPlane) (PLANE_U + uv), img, inp, b8); //======= 8x8 trannsform size & CABAC ========
            }
            else //4x4
            {
              //======= Other Modes & CABAC ========
              //------------------------------------
              for (j=block_y; j < block_y+2; j++)
              {
                for (i=block_x; i < block_x+2; i++)
                {
                  start_scan = IS_NEWINTRA (currMB)? 1 : 0;

                  img->subblock_x = i; // position for coeff_count ctx
                  img->subblock_y = j; // position for coeff_count ctx

                  if (cbp & (1<<b8))  // are there any coeff in current block at all
                  {
                    coef_ctr = start_scan - 1;
                    level    = 1;
                    img->is_intra_block = IS_INTRA(currMB);

                    if(!lossless_qpprime)
                    {
                      for(k=start_scan;(k<17) && (level!=0);k++)
                      {
                        /*
                        * make distinction between INTRA and INTER coded
                        * luminance coefficients
                        */

                        if (uv == 0)
                          currSE.context = (IS_NEWINTRA(currMB) ? CB_16AC: CB_4x4);
                        else
                          currSE.context = (IS_NEWINTRA(currMB) ? CR_16AC: CR_4x4);

						if( IS_INDEPENDENT(img) )
                          currSE.context = (IS_NEWINTRA(currMB) ? LUMA_16AC: LUMA_4x4);

                        currSE.type         = (img->is_intra_block 
                          ? (k==0 ? SE_LUM_DC_INTRA : SE_LUM_AC_INTRA) 
                          : (k==0 ? SE_LUM_DC_INTER : SE_LUM_AC_INTER)); 


#if TRACE
                        if (uv == 0)
                          sprintf(currSE.tracestring, "Cb   sng ");
                        else
                          sprintf(currSE.tracestring, "Cr   sng ");  
#endif

                        dP = &(currSlice->partArr[partMap[currSE.type]]);

                        if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)  
                          currSE.mapping = linfo_levrun_inter;
                        else                                                     
                          currSE.reading = readRunLevel_CABAC;


                        dP->readSyntaxElement(&currSE,img,dP);
                        level = currSE.value1;
                        run   = currSE.value2;
                        len   = currSE.len;

                        if (level != 0)    /* leave if len=1 */
                        {
                          coef_ctr += run+1;

                          i0=pos_scan4x4[coef_ctr][0];
                          j0=pos_scan4x4[coef_ctr][1];
                          currMB->cbp_blk_CbCr[uv] |= (int64)1 << ((j<<2) + i) ;
                          img->cof[uv + 1][(j << 2) + j0][(i << 2) + i0] = rshift_rnd_sf((level*InvLevelScale4x4[j0][i0]) << qp_per_uv[uv], 4); //444_TEMP_NOTE
                        }//level != 0
                      } //k loop
                    }//!lossless

                    else //(lossless_qpprime)
                    {
                      for(k=start_scan;(k<17) && (level!=0);k++)
                      {
                        /*
                        * make distinction between INTRA and INTER coded
                        * luminance coefficients
                        */
                        if (uv == 0)
                          currSE.context = (IS_NEWINTRA(currMB) ? CB_16AC: CB_4x4);
                        else
                          currSE.context = (IS_NEWINTRA(currMB) ? CR_16AC: CR_4x4);

						if( IS_INDEPENDENT(img) )
                          currSE.context = (IS_NEWINTRA(currMB) ? LUMA_16AC: LUMA_4x4);

                        currSE.type         = (img->is_intra_block 
                          ? (k==0 ? SE_LUM_DC_INTRA : SE_LUM_AC_INTRA) 
                          : (k==0 ? SE_LUM_DC_INTER : SE_LUM_AC_INTER));                                      

#if TRACE
                        if (uv == 0)
                          sprintf(currSE.tracestring, "Cb   sng ");
                        else
                          sprintf(currSE.tracestring, "Cr   sng ");  
#endif
                        dP = &(currSlice->partArr[partMap[currSE.type]]);

                        if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)  
                          currSE.mapping = linfo_levrun_inter;
                        else                                                     
                          currSE.reading = readRunLevel_CABAC;

                        dP->readSyntaxElement(&currSE,img,dP);
                        level = currSE.value1;
                        run   = currSE.value2;
                        len   = currSE.len;

                        if (level != 0)    /* leave if len=1 */
                        {
                          coef_ctr += run+1;

                          i0=pos_scan4x4[coef_ctr][0];
                          j0=pos_scan4x4[coef_ctr][1];
                          currMB->cbp_blk_CbCr[uv] |= (int64)1 << ((j<<2) + i);
                          img->cof[uv + 1][(j << 2) + j0][(i << 2) + i0] = level;
                        } //level != 0
                      }//k loop
                    } //lossless                    
                  } //(cbp & (1<<b8))                   
                } //i=block_x
              }//j=block_y              
            } //4x4            
          } //CABAC      
        } //block_x 
      } //block_y      
    } //uv loop    
  } //444
  
  if ((dec_picture->chroma_format_idc != YUV400) && (dec_picture->chroma_format_idc != YUV444))
  {
    memset(&img->cof[1][0][0], 0, MB_PIXELS * sizeof(int));
    memset(&img->cof[2][0][0], 0, MB_PIXELS * sizeof(int));

    //========================== CHROMA DC ============================
    //-----------------------------------------------------------------
    // chroma DC coeff
    if(cbp>15)
    {
      if (dec_picture->chroma_format_idc == YUV420)
      {
        for (ll=0;ll<3;ll+=2)
        {
          uv = ll>>1;
          {

            int (*InvLevelScale4x4Chroma)[4] = intra
              ? InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv[uv]] 
              : InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv[uv]];

              //===================== CHROMA DC YUV420 ======================
              memset(&img->cofu[0], 0, 4 *sizeof(int));

              if (active_pps->entropy_coding_mode_flag == UVLC)
              {
                readCoeff4x4_CAVLC(currMB, img, inp, CHROMA_DC, 0, 0, levarr, runarr, &numcoeff);
                coef_ctr=-1;
                level=1;
                for(k = 0; k < numcoeff; k++)
                {
                  if (levarr[k] != 0)
                  {
                    currMB->cbp_blk |= 0xf0000 << (ll<<1) ;
                    coef_ctr += runarr[k] + 1;
                    img->cofu[coef_ctr]=levarr[k];
                  }
                }
              }
              else
              {
                coef_ctr=-1;
                level=1;
                for(k=0;(k<(img->num_cdc_coeff+1))&&(level!=0);k++)
                {
                  currSE.context      = CHROMA_DC;
                  currSE.type         = (IS_INTRA(currMB) ? SE_CHR_DC_INTRA : SE_CHR_DC_INTER);
                  img->is_intra_block =  IS_INTRA(currMB);
                  img->is_v_block     = ll;

#if TRACE
                  snprintf(currSE.tracestring, TRACESTRING_SIZE, "2x2 DC Chroma ");
#endif
                  dP = &(currSlice->partArr[partMap[currSE.type]]);

                  if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
                    currSE.mapping = linfo_levrun_c2x2;
                  else
                    currSE.reading = readRunLevel_CABAC;

                  dP->readSyntaxElement(&currSE,img,dP);
                  level = currSE.value1;
                  run = currSE.value2;
                  len = currSE.len;
                  if (level != 0)
                  {
                    currMB->cbp_blk |= 0xf0000 << (ll<<1) ;
                    coef_ctr += run + 1;
                    // Bug: img->cofu has only 4 entries, hence coef_ctr MUST be <4 (which is
                    // caught by the assert().  If it is bigger than 4, it starts patching the
                    // img->predmode pointer, which leads to bugs later on.
                    //
                    // This assert() should be left in the code, because it captures a very likely
                    // bug early when testing in error prone environments (or when testing NAL
                    // functionality).
                    assert (coef_ctr < img->num_cdc_coeff);
                    img->cofu[coef_ctr]=level;
                  }
                }
              }

              if (smb // check to see if MB type is SPred or SIntra4x4
                || lossless_qpprime)
              {                
                img->cof[uv + 1][0][0]=img->cofu[0];
                img->cof[uv + 1][0][4]=img->cofu[1];
                img->cof[uv + 1][4][0]=img->cofu[2];
                img->cof[uv + 1][4][4]=img->cofu[3];
              }
              else
              {
                temp[0]=(img->cofu[0]+img->cofu[1]+img->cofu[2]+img->cofu[3]);
                temp[1]=(img->cofu[0]-img->cofu[1]+img->cofu[2]-img->cofu[3]);
                temp[2]=(img->cofu[0]+img->cofu[1]-img->cofu[2]-img->cofu[3]);
                temp[3]=(img->cofu[0]-img->cofu[1]-img->cofu[2]+img->cofu[3]);

                for (i=0;i<img->num_cdc_coeff;i++)
                {
                  if(qp_per_uv[uv]<5)
                  {
                    temp[i]=(temp[i]*InvLevelScale4x4Chroma[0][0])>>(5-qp_per_uv[uv]);
                  }
                  else
                  {
                    temp[i]=(temp[i]*InvLevelScale4x4Chroma[0][0])<<(qp_per_uv[uv]-5);
                  }
                }
                img->cof[uv + 1][0][0]=temp[0];
                img->cof[uv + 1][0][4]=temp[1];
                img->cof[uv + 1][4][0]=temp[2];
                img->cof[uv + 1][4][4]=temp[3];
              }
          }
        }
      }
      else if (dec_picture->chroma_format_idc == YUV422)
      {
        for (ll=0;ll<3;ll+=2)
        {
          uv = ll>>1;
          {
            int i,j;
            int m3[2][4] = {{0,0,0,0},{0,0,0,0}};
            int m4[2][4] = {{0,0,0,0},{0,0,0,0}};
            int qp_per_uv_dc = (currMB->qpc[uv] + 3 + img->bitdepth_chroma_qp_scale)/6;       //for YUV422 only
            int qp_rem_uv_dc = (currMB->qpc[uv] + 3 + img->bitdepth_chroma_qp_scale)%6;       //for YUV422 only

            //===================== CHROMA DC YUV422 ======================
            if (active_pps->entropy_coding_mode_flag == UVLC)
            {
              readCoeff4x4_CAVLC(currMB, img, inp, CHROMA_DC, 0, 0, levarr, runarr, &numcoeff);
              coef_ctr=-1;
              level=1;
              for(k = 0; k < numcoeff; k++)
              {
                if (levarr[k] != 0)
                {
                  currMB->cbp_blk |= ((int64)0xff0000) << (ll<<2);
                  coef_ctr=coef_ctr+runarr[k]+1;
                  i0 = SCAN_YUV422[coef_ctr][0];
                  j0 = SCAN_YUV422[coef_ctr][1];

                  m3[i0][j0]=levarr[k];
                }
              }
            }
            else
            {
              coef_ctr=-1;
              level=1;
              for(k=0;(k<9)&&(level!=0);k++)
              {
                currSE.context      = CHROMA_DC_2x4;
                currSE.type         = (IS_INTRA(currMB) ? SE_CHR_DC_INTRA : SE_CHR_DC_INTER);
                img->is_intra_block =  IS_INTRA(currMB);
                img->is_v_block     = ll;

#if TRACE
                snprintf(currSE.tracestring, TRACESTRING_SIZE, "2x4 DC Chroma ");
#endif
                dP = &(currSlice->partArr[partMap[currSE.type]]);

                if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
                  currSE.mapping = linfo_levrun_c2x2;
                else
                  currSE.reading = readRunLevel_CABAC;

                dP->readSyntaxElement(&currSE,img,dP);
                level = currSE.value1;
                run = currSE.value2;
                len = currSE.len;
                if (level != 0)
                {
                  currMB->cbp_blk |= ((int64)0xff0000) << (ll<<2) ;
                  coef_ctr=coef_ctr+run+1;
                  assert (coef_ctr < img->num_cdc_coeff);
                  i0=SCAN_YUV422[coef_ctr][0];
                  j0=SCAN_YUV422[coef_ctr][1];

                  m3[i0][j0]=level;
                }
              }
            }
            // inverse CHROMA DC YUV422 transform
            // horizontal
            if(!lossless_qpprime)
            {
              m4[0][0] = m3[0][0] + m3[1][0];
              m4[0][1] = m3[0][1] + m3[1][1];
              m4[0][2] = m3[0][2] + m3[1][2];
              m4[0][3] = m3[0][3] + m3[1][3];

              m4[1][0] = m3[0][0] - m3[1][0];
              m4[1][1] = m3[0][1] - m3[1][1];
              m4[1][2] = m3[0][2] - m3[1][2];
              m4[1][3] = m3[0][3] - m3[1][3];
            }
            else
            {
              for(i=0;i<2;i++)
                for(j=0;j<4;j++)
                  img->cof[uv + 1][j<<2][i<<2]=m3[i][j];
            }

            // vertical
            for (i=0;i<2 && !lossless_qpprime;i++)
            {
              // This is not right with the 16x16 mtr structure. Needs revisit... AMT
              int (*imgcof)[16] = img->cof[uv + 1];
              for (j=0; j < 4;j++)    //TODO: remove m5 with m4
                m5[j]=m4[i][j];

              m6[0]=m5[0]+m5[2];
              m6[1]=m5[0]-m5[2];
              m6[2]=m5[1]-m5[3];
              m6[3]=m5[1]+m5[3];


              if(qp_per_uv_dc<4)
              {
                if(intra == 1)
                {
                  imgcof[ 0][i<<2] = ((((m6[0]+m6[3])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                  imgcof[ 4][i<<2] = ((((m6[1]+m6[2])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                  imgcof[ 8][i<<2] = ((((m6[1]-m6[2])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                  imgcof[12][i<<2] = ((((m6[0]-m6[3])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                }
                else
                {
                  imgcof[ 0][i<<2] = ((((m6[0]+m6[3])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                  imgcof[ 4][i<<2] = ((((m6[1]+m6[2])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                  imgcof[ 8][i<<2] = ((((m6[1]-m6[2])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                  imgcof[12][i<<2] = ((((m6[1]-m6[2])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0]+(1<<(3-qp_per_uv_dc)))>>(4-qp_per_uv_dc))+2)>>2;
                }
              }
              else
              {
                if(intra == 1)
                {
                  imgcof[ 0][i<<2] = ((((m6[0]+m6[3])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                  imgcof[ 4][i<<2] = ((((m6[1]+m6[2])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                  imgcof[ 8][i<<2] = ((((m6[1]-m6[2])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                  imgcof[12][i<<2] = ((((m6[0]-m6[3])*InvLevelScale4x4Chroma_Intra[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                }
                else
                {
                  imgcof[ 0][i<<2] = ((((m6[0]+m6[3])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                  imgcof[ 4][i<<2] = ((((m6[1]+m6[2])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                  imgcof[ 8][i<<2] = ((((m6[1]-m6[2])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                  imgcof[12][i<<2] = ((((m6[0]-m6[3])*InvLevelScale4x4Chroma_Inter[uv][qp_rem_uv_dc][0][0])<<(qp_per_uv_dc-4))+2)>>2;
                }
              }
            }//for (i=0;i<2;i++)
          }
        }//for (ll=0;ll<3;ll+=2)
      }//else if (dec_picture->chroma_format_idc == YUV422)
    }

    // chroma AC coeff, all zero fram start_scan
    if (cbp<=31)
      for (i=0; i < 4; i++)
        memset(&img->nz_coeff [mb_nr ][i][4], 0, img->num_blk8x8_uv * sizeof(int));


    //========================== CHROMA AC ============================
    //-----------------------------------------------------------------
    // chroma AC coeff, all zero fram start_scan
    if (cbp>31)
    {
      if (active_pps->entropy_coding_mode_flag == UVLC)
      {
        if(!lossless_qpprime)
        {
          for (b8=0; b8 < img->num_blk8x8_uv; b8++)
          {
            int uvc = (b8 > ((img->num_uv_blocks) - 1 ));
            int (*InvLevelScale4x4Chroma)[4] = intra
              ? InvLevelScale4x4Chroma_Intra[uvc][qp_rem_uv[uvc]] 
              : InvLevelScale4x4Chroma_Inter[uvc][qp_rem_uv[uvc]];

              img->is_v_block = uv = uvc;

              for (b4=0; b4 < 4; b4++)
              {
                i = cofuv_blk_x[yuv][b8][b4];
                j = cofuv_blk_y[yuv][b8][b4];

                readCoeff4x4_CAVLC(currMB, img, inp, CHROMA_AC, i + 2*uvc, j + 4, levarr, runarr, &numcoeff);
                coef_ctr=0;
                level=1;

                for(k = 0; k < numcoeff;k++)
                {
                  if (levarr[k] != 0)
                  {
                    currMB->cbp_blk |= ((int64)1) << cbp_blk_chroma[b8][b4];
                    coef_ctr += runarr[k] + 1;

                    i0=pos_scan4x4[coef_ctr][0];
                    j0=pos_scan4x4[coef_ctr][1];

                    img->cof[uv + 1][(j<<2) + j0][(i<<2) + i0] = rshift_rnd_sf((levarr[k]*InvLevelScale4x4Chroma[j0][i0])<<qp_per_uv[uv], 4);
                  }
                }
              }
          }
        }
        else
        {
          for (b8=0; b8 < img->num_blk8x8_uv; b8++)
          {
            int uvc = (b8 > ((img->num_uv_blocks) - 1 ));

            img->is_v_block = uv = uvc;

            for (b4=0; b4 < 4; b4++)
            {
              i = cofuv_blk_x[yuv][b8][b4];
              j = cofuv_blk_y[yuv][b8][b4];

              readCoeff4x4_CAVLC(currMB, img, inp, CHROMA_AC, i, j, levarr, runarr, &numcoeff);
              coef_ctr=0;
              level=1;

              for(k = 0; k < numcoeff;k++)
              {
                if (levarr[k] != 0)
                {
                  currMB->cbp_blk |= ((int64)1) << cbp_blk_chroma[b8][b4];
                  coef_ctr += runarr[k]+1;

                  i0=pos_scan4x4[coef_ctr][0];
                  j0=pos_scan4x4[coef_ctr][1];

                  img->cof[uv + 1][(j<<2) + j0][(i<<2) + i0] = levarr[k];
                }
              }
            }
          }
        }
      }
      else
      {
        currSE.context      = CHROMA_AC;
        currSE.type         = (IS_INTRA(currMB) ? SE_CHR_AC_INTRA : SE_CHR_AC_INTER);
        img->is_intra_block =  IS_INTRA(currMB);

        if(!lossless_qpprime)
        {
          for (b8=0; b8 < img->num_blk8x8_uv; b8++)
          {
            int uvc = (b8 > ((img->num_uv_blocks) - 1 ));
            int (*InvLevelScale4x4Chroma)[4] = intra
              ? InvLevelScale4x4Chroma_Intra[uvc][qp_rem_uv[uvc]] 
              : InvLevelScale4x4Chroma_Inter[uvc][qp_rem_uv[uvc]];

              img->is_v_block = uv = uvc;

              for (b4=0; b4 < 4; b4++)
              {
                i = cofuv_blk_x[yuv][b8][b4];
                j = cofuv_blk_y[yuv][b8][b4];

                coef_ctr=0;
                level=1;

                img->subblock_y = subblk_offset_y[yuv][b8][b4]>>2;
                img->subblock_x = subblk_offset_x[yuv][b8][b4]>>2;

                for(k=0;(k<16)&&(level!=0);k++)
                {
#if TRACE
                  snprintf(currSE.tracestring, TRACESTRING_SIZE, "AC Chroma ");
#endif
                  dP = &(currSlice->partArr[partMap[currSE.type]]);

                  if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
                    currSE.mapping = linfo_levrun_inter;
                  else
                    currSE.reading = readRunLevel_CABAC;

                  dP->readSyntaxElement(&currSE,img,dP);
                  level = currSE.value1;
                  run = currSE.value2;
                  len = currSE.len;

                  if (level != 0)
                  {
                    currMB->cbp_blk |= ((int64)1) << cbp_blk_chroma[b8][b4];
                    coef_ctr += (run + 1);

                    i0=pos_scan4x4[coef_ctr][0];
                    j0=pos_scan4x4[coef_ctr][1];

                    img->cof[uv + 1][(j<<2) + j0][(i<<2) + i0] = rshift_rnd_sf((level*InvLevelScale4x4Chroma[j0][i0])<<qp_per_uv[uv], 4);
                  }
                } //for(k=0;(k<16)&&(level!=0);k++)
              }
          }
        }
        else
        {
          for (b8=0; b8 < img->num_blk8x8_uv; b8++)
          {
            int uvc = (b8 > ((img->num_uv_blocks) - 1 ));

            img->is_v_block = uv = uvc;

            for (b4=0; b4 < 4; b4++)
            {
              i = cofuv_blk_x[yuv][b8][b4];
              j = cofuv_blk_y[yuv][b8][b4];

              coef_ctr=0;
              level=1;

              img->subblock_y = subblk_offset_y[yuv][b8][b4]>>2;
              img->subblock_x = subblk_offset_x[yuv][b8][b4]>>2;

              for(k=0;(k<16)&&(level!=0);k++)
              {
#if TRACE
                snprintf(currSE.tracestring, TRACESTRING_SIZE, "AC Chroma ");
#endif
                dP = &(currSlice->partArr[partMap[currSE.type]]);

                if (active_pps->entropy_coding_mode_flag == UVLC || dP->bitstream->ei_flag)
                  currSE.mapping = linfo_levrun_inter;
                else
                  currSE.reading = readRunLevel_CABAC;

                dP->readSyntaxElement(&currSE,img,dP);
                level = currSE.value1;
                run = currSE.value2;
                len = currSE.len;

                if (level != 0)
                {
                  currMB->cbp_blk |= ((int64)1) << cbp_blk_chroma[b8][b4];
                  coef_ctr += (run + 1);

                  i0=pos_scan4x4[coef_ctr][0];
                  j0=pos_scan4x4[coef_ctr][1];

                  img->cof[uv + 1][(j<<2) + j0][(i<<2) + i0] = level;
                }
              } 
            }
          } 
        } //for (b4=0; b4 < 4; b4++)
      } //for (b8=0; b8 < img->num_blk8x8_uv; b8++)
    } //if (dec_picture->chroma_format_idc != YUV400)
  }
}

/*!
 ************************************************************************
 * \brief
 *    Copy IPCM coefficients to decoded picture buffer and set parameters for this MB
 *    (for IPCM CABAC and IPCM CAVLC  28/11/2003)
 *
 * \author
 *    Dong Wang <Dong.Wang@bristol.ac.uk>
 ************************************************************************
 */

void decode_ipcm_mb(Macroblock *currMB, struct img_par *img)
{
  int i,j;
  int mb_nr = img->current_mb_nr;

  //Copy coefficients to decoded picture buffer
  //IPCM coefficients are stored in img->cof which is set in function readIPCMcoeffsFromNAL()

  for(i=0;i<16;i++)
    for(j=0;j<16;j++)
      dec_picture->imgY[img->pix_y+i][img->pix_x+j]=img->cof[0][i][j];

  if ((dec_picture->chroma_format_idc != YUV400) && !IS_INDEPENDENT(img))
  {
    for(i=0;i<img->mb_cr_size_y;i++)
      for(j=0;j<img->mb_cr_size_x;j++)
        dec_picture->imgUV[0][img->pix_c_y+i][img->pix_c_x+j]=img->cof[1][i][j];  

    for(i=0;i<img->mb_cr_size_y;i++)
      for(j=0;j<img->mb_cr_size_x;j++)
        dec_picture->imgUV[1][img->pix_c_y+i][img->pix_c_x+j]=img->cof[2][i][j];  
  }

  // for deblocking filter
  currMB->qp=0;
  set_chroma_qp(currMB);

  // for CAVLC: Set the nz_coeff to 16.
  // These parameters are to be used in CAVLC decoding of neighbour blocks
  for(i=0;i<4;i++)
    for (j=0;j<(4 + img->num_blk8x8_uv);j++)
      img->nz_coeff[mb_nr][i][j]=16;


  // for CABAC decoding of MB skip flag
  currMB->skip_flag = 0;

  //for deblocking filter CABAC
  currMB->cbp_blk=0xFFFF;

  //For CABAC decoding of Dquant
  last_dquant=0;
}

/*!
 ************************************************************************
 * \brief
 *    decode one macroblock
 ************************************************************************
 */

int decode_one_macroblock(Macroblock *currMB, struct img_par *img,struct inp_par *inp)
{
  int i=0,j=0,k,l,ii=0,jj=0, j4=0,i4=0;  
  int uv, hv;
  int ioff,joff;
  int block8x8;   // needed for ABT
    int j_pos, i_pos;
  
  static const byte decode_block_scan[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
  int mb_nr     = img->current_mb_nr;
  
  short ref_idx, l0_refframe=-1, l1_refframe=-1;
  int mv_mode, pred_dir; // = currMB->ref_frame;
  short l0_ref_idx=-1, l1_ref_idx=-1;
  
  short  *** mv_array = NULL, ***l0_mv_array = NULL, ***l1_mv_array = NULL;
  int block_size_x, block_size_y;
  
  int mv_scale;
  static imgpel **curComp;
  static imgpel (*mpr) [16];
  static imgpel *cur_line;
  static int    *cur_m7;
  
  
  int smb = ((img->type==SP_SLICE) && IS_INTER (currMB)) || (img->type == SI_SLICE && currMB->mb_type == SI4MB);
  int list_offset;
  int max_y_cr;
  
  char l0_rFrame = -1, l1_rFrame = -1;
  
  short pmvl0[2]={0,0}, pmvl1[2]={0,0};
  
  int direct_pdir=-1;
  
  int curr_mb_field = ((img->MbaffFrameFlag)&&(currMB->mb_field));
  
  static byte  **    moving_block;
  static short ****  co_located_mv;
  static char  ***   co_located_ref_idx;
  static int64 ***   co_located_ref_id;
  
  int need_4x4_transform = (!currMB->luma_transform_size_8x8_flag);
  int yuv = dec_picture->chroma_format_idc - 1;

  //For residual DPCM
  Boolean lossless_qpprime = (Boolean) (((img->qp + img->bitdepth_luma_qp_scale) == 0) && (img->lossless_qpprime_flag == 1));  
  ipmode_DPCM = NO_INTRA_PMODE; 
  
  if(img->type==SP_SLICE && currMB->mb_type!=I16MB)
    smb = 1;// modif ES added
  
  if(currMB->mb_type == IPCM)
  {
    //copy readed data into imgY and set parameters
    decode_ipcm_mb(currMB, img);
    return 0;
  }
  
  //////////////////////////
  
  // find out the correct list offsets
  if (curr_mb_field)
  {
    if(mb_nr&0x01)
    {
      list_offset = 4; // top field mb
      moving_block = Co_located->bottom_moving_block;
      co_located_mv = Co_located->bottom_mv;
      co_located_ref_idx = Co_located->bottom_ref_idx;
      co_located_ref_id = Co_located->bottom_ref_pic_id;
    }
    else
    {
      list_offset = 2; // bottom field mb
      moving_block = Co_located->top_moving_block;
      co_located_mv = Co_located->top_mv;
      co_located_ref_idx = Co_located->top_ref_idx;
      co_located_ref_id = Co_located->top_ref_pic_id;
    }
    max_y_cr = (dec_picture->size_y_cr>>1)-1;
  }
  else
  {
    list_offset = 0;  // no mb aff or frame mb
    moving_block = Co_located->moving_block;
    co_located_mv = Co_located->mv;
    co_located_ref_idx = Co_located->ref_idx;
    co_located_ref_id = Co_located->ref_pic_id;
    max_y_cr = dec_picture->size_y_cr-1;
  }
  
  if (!img->MbaffFrameFlag)
  {
    for (l = LIST_0 + list_offset; l <= (LIST_1 + list_offset); l++)
    {
      for(k = 0; k < listXsize[l]; k++)
      {
        listX[l][k]->chroma_vector_adjustment= 0;
        if(img->structure == TOP_FIELD && img->structure != listX[l][k]->structure)
          listX[l][k]->chroma_vector_adjustment = -2;
        if(img->structure == BOTTOM_FIELD && img->structure != listX[l][k]->structure)
          listX[l][k]->chroma_vector_adjustment = 2;
      }
    }
  }
  else
  {
    if (curr_mb_field)
    {
      for (l = LIST_0 + list_offset; l <= (LIST_1 + list_offset); l++)
      {
        for(k = 0; k < listXsize[l]; k++)
        {
          listX[l][k]->chroma_vector_adjustment= 0;
          if(mb_nr % 2 == 0 && listX[l][k]->structure == BOTTOM_FIELD)
            listX[l][k]->chroma_vector_adjustment = -2;
          if(mb_nr % 2 == 1 && listX[l][k]->structure == TOP_FIELD)
            listX[l][k]->chroma_vector_adjustment = 2;
        }
      }
    }
    else
    {
      for (l = LIST_0 + list_offset; l <= (LIST_1 + list_offset); l++)
      {
        for(k = 0; k < listXsize[l]; k++)
        {
          listX[l][k]->chroma_vector_adjustment= 0;
        }
      }
    }
  }
  
  // luma decoding **************************************************
  
  // get prediction for INTRA_MB_16x16
  if (IS_NEWINTRA (currMB))
  {
    intrapred_luma_16x16(currMB, PLANE_Y, img, currMB->i16mode);
    ipmode_DPCM = currMB->i16mode; //For residual DPCM
    // =============== 4x4 itrans ================
    // -------------------------------------------
    iMBtrans4x4(PLANE_Y, img, smb);
    
    // chroma decoding *******************************************************
    if ((dec_picture->chroma_format_idc != YUV400) && (dec_picture->chroma_format_idc != YUV444)) 
    {
      intra_cr_decoding(currMB, yuv, img, smb);
    }
  }
  else if (currMB->mb_type == I4MB)
  {
    for (block8x8 = 0; block8x8 < 4; block8x8++)
    {
      for (k = block8x8 * 4; k < block8x8 * 4 + 4; k ++)
      {
        i =  (decode_block_scan[k] & 3);
        j = ((decode_block_scan[k] >> 2) & 3);
        
        ioff = (i << 2);
        joff = (j << 2);
        i4   = img->block_x + i;
        j4   = img->block_y + j;
        j_pos = j4 * BLOCK_SIZE;
        i_pos = i4 * BLOCK_SIZE;
        
        // PREDICTION
        //===== INTRA PREDICTION =====
        if (intrapred(currMB, PLANE_Y, img,ioff,joff,i4,j4) == SEARCH_SYNC)  /* make 4x4 prediction block mpr from given prediction img->mb_mode */
          return SEARCH_SYNC;                   /* bit error */
        // =============== 4x4 itrans ================
        // -------------------------------------------
        //  itrans4x4   (img, ioff, joff, LumaComp);      // use DCT transform and make 4x4 block m7 from prediction block mpr

        if(!lossless_qpprime)  //For residual DPCM
          itrans4x4   (img, ioff, joff, LumaComp);      // use DCT transform and make 4x4 block m7 from prediction block mpr
        else
          Inv_Residual_trans_4x4(img, ioff, joff, i, j, 0, LumaComp);
        
        for(jj=0;jj<BLOCK_SIZE;jj++)
        {
          cur_m7 = &img->m7[0][jj + joff][ioff];
          cur_line = &dec_picture->imgY[j_pos + jj][i_pos];
          for(ii=0;ii<BLOCK_SIZE;ii++)
          {
            *(cur_line++) = (*cur_m7++); // construct picture from 4x4 blocks
          }
        }
      }
    }
    
    // chroma decoding *******************************************************
    if ((dec_picture->chroma_format_idc != YUV400) && (dec_picture->chroma_format_idc != YUV444)) 
    {
      intra_cr_decoding(currMB, yuv, img, smb);
    }
  }
  else if (currMB->mb_type == I8MB) 
  {
    for (block8x8 = 0; block8x8 < 4; block8x8++)
    {
      //=========== 8x8 BLOCK TYPE ============
      ioff = 8 * (block8x8 & 0x01);
      joff = 8 * (block8x8 >> 1);
      
      //PREDICTION
      intrapred8x8(currMB, PLANE_Y, img, block8x8);
  //    itrans8x8(PLANE_Y, img,ioff,joff);      // use DCT transform and make 8x8 block m7 from prediction block mpr
      if(!lossless_qpprime)   //For residual DPCM
        itrans8x8(PLANE_Y, img,ioff,joff);      // use DCT transform and make 8x8 block m7 from prediction block mpr
      else
        Inv_Residual_trans_8x8(PLANE_Y, img,ioff,joff);
      
      for(jj = joff; jj < joff + 8;jj++)
      {
        cur_m7 = &img->m7[0][jj][ioff];
        cur_line = &dec_picture->imgY[img->pix_y + jj][img->pix_x + ioff];

        for(ii = 0; ii < 8; ii++)
        {
          *(cur_line++) = *(cur_m7++); // construct picture from 8x8 blocks
        }
      }
    } 
    // chroma decoding *******************************************************
    if ((dec_picture->chroma_format_idc != YUV400) && (dec_picture->chroma_format_idc != YUV444)) 
    {
      intra_cr_decoding(currMB, yuv, img, smb);
    }
  }
  else if ((img->type == P_SLICE) && (currMB->mb_type == PSKIP))
  {   
    block_size_x = MB_BLOCK_SIZE;
    block_size_y = MB_BLOCK_SIZE;
    i = 0;
    j = 0;
    
    pred_dir = LIST_0;   
    perform_mc(currMB, PLANE_Y, dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
    
    mpr = img->mpr[LumaComp];
    curComp = &dec_picture->imgY[img->pix_y];
    for(j = 0; j < img->mb_size[0][1]; j++)
    {                
      memcpy(&(curComp[j][img->pix_x]), &(mpr[j][0]), img->mb_size[0][0] * sizeof(imgpel));
    }

    if ((dec_picture->chroma_format_idc != YUV400) && (dec_picture->chroma_format_idc != YUV444)) 
    {
      for(uv=0;uv<2;uv++)
      {
        curComp = &dec_picture->imgUV[uv][img->pix_c_y]; 
        
        mpr = img->mpr[uv + 1];
        for(jj = 0; jj < img->mb_size[1][1]; jj++)
          memcpy(&(curComp[jj][img->pix_c_x]), &(mpr[jj][0]), img->mb_size[1][0] * sizeof(imgpel));
      }
    }
  }
  else if (currMB->mb_type == P16x16)
  {
    block_size_x = MB_BLOCK_SIZE;
    block_size_y = MB_BLOCK_SIZE;
    i = 0;
    j = 0;
    
    pred_dir = currMB->b8pdir[0];   
    perform_mc(currMB, PLANE_Y, dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
    iTransform(currMB, PLANE_Y, img, need_4x4_transform, smb, yuv);    
  }
  else if (currMB->mb_type == P16x8)
  {   
    block_size_x = MB_BLOCK_SIZE;
    block_size_y = 8;    
    
    for (block8x8 = 0; block8x8 < 4; block8x8 += 2)
    {
      i = 0;
      j = block8x8;      
      
      pred_dir = currMB->b8pdir[block8x8];
      perform_mc(currMB, PLANE_Y, dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
    }
    iTransform(currMB, PLANE_Y, img, need_4x4_transform, smb, yuv); 
  }
  else if (currMB->mb_type == P8x16)
  {   
    
    block_size_x = 8;
    block_size_y = 16;
    
    for (block8x8 = 0; block8x8 < 2; block8x8 ++)
    {
      i = block8x8<<1;
      j = 0;      
      pred_dir = currMB->b8pdir[block8x8];
      assert (pred_dir<=2);
      perform_mc(currMB, PLANE_Y, dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
    }
    iTransform(currMB, PLANE_Y, img, need_4x4_transform, smb, yuv);
  }
  else
  {
    // prepare direct modes
    if (img->type==B_SLICE && img->direct_spatial_mv_pred_flag && (IS_DIRECT (currMB) ||
      (IS_P8x8(currMB) && !(currMB->b8mode[0] && currMB->b8mode[1] && currMB->b8mode[2] && currMB->b8mode[3]))))
      prepare_direct_params(currMB, dec_picture, img, pmvl0, pmvl1, &l0_rFrame, &l1_rFrame);
    
    for (block8x8=0; block8x8<4; block8x8++)
    {
      mv_mode  = currMB->b8mode[block8x8];
      pred_dir = currMB->b8pdir[block8x8];
      
      //if ( mv_mode == SMB8x8 || mv_mode == SMB8x4 || mv_mode == SMB4x8 || mv_mode == SMB4x4 )
      if ( mv_mode != 0 )
      {
        int k_start = (block8x8 << 2);
        int k_inc = (mv_mode == SMB8x4) ? 2 : 1;
        int k_end = (mv_mode == SMB8x8) ? k_start + 1 : ((mv_mode == SMB4x4) ? k_start + 4 : k_start + k_inc + 1);
        
        block_size_x = ( mv_mode == SMB8x4 || mv_mode == SMB8x8 ) ? SMB_BLOCK_SIZE : BLOCK_SIZE;
        block_size_y = ( mv_mode == SMB4x8 || mv_mode == SMB8x8 ) ? SMB_BLOCK_SIZE : BLOCK_SIZE;
        
        for (k = k_start; k < k_end; k += k_inc)
        {
          i =  (decode_block_scan[k] & 3);
          j = ((decode_block_scan[k] >> 2) & 3);
          perform_mc(currMB, PLANE_Y, dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
        }        
      }
      else
      {
        int k_start = (block8x8 << 2);
        int k_end = k_start;
        
        if (active_sps->direct_8x8_inference_flag)
        {
          block_size_x = SMB_BLOCK_SIZE;
          block_size_y = SMB_BLOCK_SIZE;
          k_end ++;
        }
        else
        {
          block_size_x = BLOCK_SIZE;
          block_size_y = BLOCK_SIZE;
          k_end += BLOCK_MULTIPLE;
        }
        
        // Prepare mvs (needed for deblocking and mv prediction
        for (k = k_start; k < k_start + BLOCK_MULTIPLE; k ++)
        {
          
          i =  (decode_block_scan[k] & 3);
          j = ((decode_block_scan[k] >> 2) & 3);
          
          ioff = (i << 2);
          i4   = img->block_x + i;
          
          joff = (j << 2);
          j4   = img->block_y + j;
          
          assert (pred_dir<=2);
          
          if (img->direct_spatial_mv_pred_flag)
          {
            int j6 = img->block_y_aff + j;
            
            //===== DIRECT PREDICTION =====
            l0_mv_array = dec_picture->mv[LIST_0];
            l1_mv_array = dec_picture->mv[LIST_1];
            l1_refframe = 0;
            
            if (l0_rFrame >=0)
            {
              if (!l0_rFrame  && ((!moving_block[j6][i4]) && (!listX[LIST_1 + list_offset][0]->is_long_term)))
              {
                dec_picture->mv  [LIST_0][j4][i4][0] = 0;
                dec_picture->mv  [LIST_0][j4][i4][1] = 0;
                dec_picture->ref_idx[LIST_0][j4][i4] = 0;
              }
              else
              {
                dec_picture->mv  [LIST_0][j4][i4][0] = pmvl0[0];
                dec_picture->mv  [LIST_0][j4][i4][1] = pmvl0[1];
                dec_picture->ref_idx[LIST_0][j4][i4] = l0_rFrame;
              }
            }
            else
            {
              dec_picture->ref_idx[LIST_0][j4][i4] = -1;
              dec_picture->mv  [LIST_0][j4][i4][0] = 0;
              dec_picture->mv  [LIST_0][j4][i4][1] = 0;
            }
            
            if (l1_rFrame >=0)
            {
              if  (l1_rFrame==0 && ((!moving_block[j6][i4]) && (!listX[LIST_1 + list_offset][0]->is_long_term)))
              {
                
                dec_picture->mv  [LIST_1][j4][i4][0] = 0;
                dec_picture->mv  [LIST_1][j4][i4][1] = 0;
                dec_picture->ref_idx[LIST_1][j4][i4] = l1_rFrame;
                
              }
              else
              {
                dec_picture->mv  [LIST_1][j4][i4][0] = pmvl1[0];
                dec_picture->mv  [LIST_1][j4][i4][1] = pmvl1[1];
                dec_picture->ref_idx[LIST_1][j4][i4] = l1_rFrame;
              }
            }
            else
            {
              dec_picture->mv  [LIST_1][j4][i4][0] = 0;
              dec_picture->mv  [LIST_1][j4][i4][1] = 0;
              dec_picture->ref_idx[LIST_1][j4][i4] = -1;
            }
            
            if (l0_rFrame < 0 && l1_rFrame < 0)
            {
              dec_picture->ref_idx[LIST_0][j4][i4] = 0;
              dec_picture->ref_idx[LIST_1][j4][i4] = 0;
            }
            
            l0_refframe = (dec_picture->ref_idx[LIST_0][j4][i4]!=-1) ? dec_picture->ref_idx[LIST_0][j4][i4] : 0;
            l1_refframe = (dec_picture->ref_idx[LIST_1][j4][i4]!=-1) ? dec_picture->ref_idx[LIST_1][j4][i4] : 0;
            
            l0_ref_idx = l0_refframe;
            l1_ref_idx = l1_refframe;
            
            if      (dec_picture->ref_idx[LIST_1][j4][i4]==-1) 
            {
              direct_pdir = 0;
              l0_refframe = ref_idx  = (dec_picture->ref_idx[LIST_0][j4][i4] != -1) ? dec_picture->ref_idx[LIST_0][j4][i4] : 0;
              mv_array = dec_picture->mv[LIST_0];
            }
            else if (dec_picture->ref_idx[LIST_0][j4][i4]==-1) 
            {
              direct_pdir = 1;
              l0_refframe = ref_idx  = (dec_picture->ref_idx[LIST_1][j4][i4] != -1) ? dec_picture->ref_idx[LIST_1][j4][i4] : 0;
              mv_array = dec_picture->mv[LIST_1];
            }
            else                                               
              direct_pdir = 2;
            
            pred_dir = direct_pdir;
            
          }
          else
          {
            int j6= img->block_y_aff + j;
            
            int refList = (co_located_ref_idx[LIST_0][j6][i4]== -1 ? LIST_1 : LIST_0);
            int ref_idx =  co_located_ref_idx[refList][j6][i4];
            l0_mv_array = dec_picture->mv[LIST_0];
            l1_mv_array = dec_picture->mv[LIST_1];
            l1_refframe = 0;
            
            if(ref_idx==-1) // co-located is intra mode
            {
              memset( &dec_picture->mv  [LIST_0][j4][i4][0], 0, 2* sizeof(short));
              memset( &dec_picture->mv  [LIST_1][j4][i4][0], 0, 2* sizeof(short));
              
              dec_picture->ref_idx[LIST_0][j4][i4] = 0;
              dec_picture->ref_idx[LIST_1][j4][i4] = 0;
              
              l0_refframe = 0;
              l0_ref_idx = 0;
            }
            else // co-located skip or inter mode
            {
              int mapped_idx=0;
              int iref;

              for (iref=0;iref<imin(img->num_ref_idx_l0_active,listXsize[LIST_0 + list_offset]);iref++)
              {
                if(img->structure==0 && curr_mb_field==0)
                {
                  // If the current MB is a frame MB and the colocated is from a field picture,
                  // then the co_located_ref_id may have been generated from the wrong value of
                  // frame_poc if it references it's complementary field, so test both POC values
                  if(listX[0][iref]->top_poc*2 == co_located_ref_id[refList][j6][i4] || listX[0][iref]->bottom_poc*2 == co_located_ref_id[refList][j6][i4])
                  {
                    mapped_idx=iref;
                    break;
                  }
                  else //! invalid index. Default to zero even though this case should not happen
                    mapped_idx=INVALIDINDEX;
                  continue;
                }

                if (dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][iref]==co_located_ref_id[refList][j6][i4])
                {
                  mapped_idx=iref;
                  break;
                }
                else //! invalid index. Default to zero even though this case should not happen
                {
                  mapped_idx=INVALIDINDEX;
                }
              }
              if (INVALIDINDEX == mapped_idx)
              {
                error("temporal direct error\ncolocated block has ref that is unavailable",-1111);
              }              

              l0_ref_idx = mapped_idx;
              mv_scale = img->mvscale[LIST_0 + list_offset][mapped_idx];

              //! In such case, an array is needed for each different reference.
              if (mv_scale == 9999 || listX[LIST_0+list_offset][mapped_idx]->is_long_term)
              {
                memcpy(&dec_picture->mv  [LIST_0][j4][i4][0], &co_located_mv[refList][j6][i4][0], 2 * sizeof(short));
                memset(&dec_picture->mv  [LIST_1][j4][i4][0], 0, 2 * sizeof(short));
              }
              else
              {
                dec_picture->mv  [LIST_0][j4][i4][0]=(mv_scale * co_located_mv[refList][j6][i4][0] + 128 ) >> 8;
                dec_picture->mv  [LIST_0][j4][i4][1]=(mv_scale * co_located_mv[refList][j6][i4][1] + 128 ) >> 8;

                dec_picture->mv  [LIST_1][j4][i4][0]=dec_picture->mv[LIST_0][j4][i4][0] - co_located_mv[refList][j6][i4][0] ;
                dec_picture->mv  [LIST_1][j4][i4][1]=dec_picture->mv[LIST_0][j4][i4][1] - co_located_mv[refList][j6][i4][1] ;
              }

              l0_refframe = dec_picture->ref_idx[LIST_0][j4][i4] = mapped_idx; //listX[1][0]->ref_idx[refList][j4][i4];
              l1_refframe = dec_picture->ref_idx[LIST_1][j4][i4] = 0;

              l0_ref_idx = l0_refframe;
              l1_ref_idx = l1_refframe;
            }
          }
          // store reference picture ID determined by direct mode
          dec_picture->ref_pic_id[LIST_0][j4][i4] = dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][(short)dec_picture->ref_idx[LIST_0][j4][i4]];
          dec_picture->ref_pic_id[LIST_1][j4][i4] = dec_picture->ref_pic_num[img->current_slice_nr][LIST_1 + list_offset][(short)dec_picture->ref_idx[LIST_1][j4][i4]];
        }
        
        for (k = k_start; k < k_end; k ++)
        {
          
          i =  (decode_block_scan[k] & 3);
          j = ((decode_block_scan[k] >> 2) & 3);
          perform_mc(currMB, PLANE_Y, dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);           
        }
      }
    }
    iTransform(currMB, PLANE_Y, img, need_4x4_transform, smb, yuv); 
  }
  
  if ((active_sps->chroma_format_idc==YUV444)&&(!IS_INDEPENDENT(img)))  
  {
    for (uv = 0; uv < 2; uv++ )
    {
    
      if (IS_NEWINTRA (currMB))
      {

        // =============== 4x4 itrans ================
        // -------------------------------------------
        if (uv==0) 
        {
          intrapred_luma_16x16(currMB, PLANE_U, img, currMB->i16mode);
          iMBtrans4x4(PLANE_U, img, smb); 
        }
        else
        
        {
          intrapred_luma_16x16(currMB, PLANE_V, img, currMB->i16mode);
          iMBtrans4x4(PLANE_V, img, smb); 
        }
      }      
      else if (currMB->mb_type == I4MB)
      {
        for (block8x8 = 0; block8x8 < 4; block8x8++)
        {
          for (k = block8x8 * 4; k < block8x8 * 4 + 4; k ++)
          {
            i =  (decode_block_scan[k] & 3);
            j = ((decode_block_scan[k] >> 2) & 3);
            ioff = (i << 2);
            joff = (j << 2);
            i4   = img->block_x + i;
            j4   = img->block_y + j;
            j_pos = j4 * BLOCK_SIZE;
            i_pos = i4 * BLOCK_SIZE;
            
            // PREDICTION
            //===== INTRA PREDICTION =====

            if (intrapred(currMB, (ColorPlane) (uv + 1), img, ioff, joff, i4,j4)==SEARCH_SYNC)  /* make 4x4 prediction block mpr from given prediction img->mb_mode */
              return SEARCH_SYNC;                   /* bit error */

            // =============== 4x4 itrans ================
            // -------------------------------------------
            //itrans4x4 (img, ioff, joff, uv + 1);      // use DCT transform and make 4x4 block m7 from prediction block mpr
            if(!lossless_qpprime)  //For residual DPCM
              itrans4x4 (img, ioff, joff, uv + 1);
            else
              Inv_Residual_trans_4x4(img,ioff,joff,i,j+4*(uv+1), 1, uv+1);
            
            for(jj=0;jj<BLOCK_SIZE;jj++)
            {
              for(ii=0;ii<BLOCK_SIZE;ii++)
              { 
                dec_picture->imgUV[uv][j_pos + jj][i_pos + ii]=img->m7[uv+1][jj + joff][ii + ioff]; // construct picture from 4x4 blocks
              }
            }
          }
        }
      } //I4MB
      
      else if (currMB->mb_type == I8MB) 
      {
        for (block8x8 = 0; block8x8 < 4; block8x8++)
        {
          //=========== 8x8 BLOCK TYPE ============
          ioff = 8 * (block8x8 & 0x01);
          joff = 8 * (block8x8 >> 1);
          
          //PREDICTION
          intrapred8x8(currMB, (ColorPlane) (uv + 1), img, block8x8); 
          // itrans8x8((ColorPlane) (uv + 1), img,ioff,joff);      // use DCT transform and make 8x8 block m7 from prediction block mpr
          //For residual DPCM
          if(!lossless_qpprime)
            itrans8x8((ColorPlane) (uv + 1), img,ioff,joff);      // use DCT transform and make 8x8 block m7 from prediction block mpr
          else
            Inv_Residual_trans_8x8((ColorPlane) (uv + 1), img,ioff,joff);

          for(jj = joff; jj < joff + 8;jj++)
          {
            for(ii = ioff; ii < ioff + 8;ii++)
            { 
              dec_picture->imgUV[uv][img->pix_y + jj][img->pix_x + ii] = img->m7[uv+1][jj][ii]; // construct picture from 4x4 blocks
            }
          }
        }
      } //I8MB
      
      else if ((img->type == P_SLICE) && (currMB->mb_type == PSKIP))
      {   
        block_size_x = MB_BLOCK_SIZE;
        block_size_y = MB_BLOCK_SIZE;
        i = 0;
        j = 0;
        
        pred_dir = LIST_0;
        perform_mc(currMB, (ColorPlane) (uv + 1), dec_picture, img,  pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
        mpr = img->mpr[uv+1];
        curComp = dec_picture->imgUV[uv];
        
        for(j = 0; j < img->mb_size[0][1]; j++)
        {                
          memcpy(&(curComp[img->pix_y + j][img->pix_x]), &(mpr[j][0]), img->mb_size[0][0] * sizeof(imgpel));
        }
      } //PSKIP
      
      else if (currMB->mb_type == P16x16)
      {
        block_size_x = MB_BLOCK_SIZE;
        block_size_y = MB_BLOCK_SIZE;
        i = 0;
        j = 0;

        pred_dir = currMB->b8pdir[0];  
        perform_mc(currMB, (ColorPlane) (uv + 1), dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
        iTransform(currMB, (ColorPlane) (uv + 1), img, need_4x4_transform, smb, yuv);   
      } //P16x16
      
      else if (currMB->mb_type == P16x8)
      {   
        block_size_x = MB_BLOCK_SIZE;
        block_size_y = 8;
        
        for (block8x8 = 0; block8x8 < 4; block8x8 += 2)
        {
          i = 0;
          j = block8x8;      
          
          pred_dir = currMB->b8pdir[block8x8];

          perform_mc(currMB, (ColorPlane) (uv + 1), dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);          
        }
        iTransform(currMB, (ColorPlane) (uv + 1), img, need_4x4_transform, smb, yuv);

      } //P16x8
      
      else if (currMB->mb_type == P8x16)
      {   
        
        block_size_x = 8;
        block_size_y = 16;
        
        for (block8x8 = 0; block8x8 < 2; block8x8 ++)
        {
          i = block8x8<<1;
          j = 0;      
          pred_dir = currMB->b8pdir[block8x8];
          assert (pred_dir<=2);
          perform_mc(currMB, (ColorPlane) (uv + 1), dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
        }

        iTransform(currMB, (ColorPlane) (uv + 1), img, need_4x4_transform, smb, yuv);
      } //P8x16
      else
      {
        // prepare direct modes
        if (img->type==B_SLICE && img->direct_spatial_mv_pred_flag && (IS_DIRECT (currMB) ||
          (IS_P8x8(currMB) && !(currMB->b8mode[0] && currMB->b8mode[1] && currMB->b8mode[2] && currMB->b8mode[3]))))
          prepare_direct_params(currMB, dec_picture, img, pmvl0, pmvl1, &l0_rFrame, &l1_rFrame);
        
        for (block8x8=0; block8x8<4; block8x8++)
        {
          mv_mode  = currMB->b8mode[block8x8];
          pred_dir = currMB->b8pdir[block8x8];
          
          //if ( mv_mode == SMB8x8 || mv_mode == SMB8x4 || mv_mode == SMB4x8 || mv_mode == SMB4x4 )
          if ( mv_mode != 0 )
          {
            int k_start = (block8x8 << 2);
            int k_inc = (mv_mode == SMB8x4) ? 2 : 1;
            int k_end = (mv_mode == SMB8x8) ? k_start + 1 : ((mv_mode == SMB4x4) ? k_start + 4 : k_start + k_inc + 1);
            
            block_size_x = ( mv_mode == SMB8x4 || mv_mode == SMB8x8 ) ? SMB_BLOCK_SIZE : BLOCK_SIZE;
            block_size_y = ( mv_mode == SMB4x8 || mv_mode == SMB8x8 ) ? SMB_BLOCK_SIZE : BLOCK_SIZE;
            
            for (k = k_start; k < k_end; k += k_inc)
            {
              i =  (decode_block_scan[k] & 3);
              j = ((decode_block_scan[k] >> 2) & 3);

              perform_mc(currMB, (ColorPlane) (uv + 1), dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
            }        
          }
          else
          {
            int k_start = (block8x8 << 2);
            int k_end = k_start;
            
            if (active_sps->direct_8x8_inference_flag)
            {
              block_size_x = SMB_BLOCK_SIZE;
              block_size_y = SMB_BLOCK_SIZE;
              k_end ++;
            }
            else
            {
              block_size_x = BLOCK_SIZE;
              block_size_y = BLOCK_SIZE;
              k_end += BLOCK_MULTIPLE;
            }
            
            // Prepare mvs (needed for deblocking and mv prediction
            for (k = k_start; k < k_start + BLOCK_MULTIPLE; k ++)
            {
              
              i =  (decode_block_scan[k] & 3);
              j = ((decode_block_scan[k] >> 2) & 3);
              
              ioff = (i << 2);
              i4   = img->block_x + i;
              
              joff = (j << 2);
              j4   = img->block_y + j;
              
              assert (pred_dir<=2);
              
              if (img->direct_spatial_mv_pred_flag)
              {
                int j6 = img->block_y_aff + j;
                
                //===== DIRECT PREDICTION =====
                l0_mv_array = dec_picture->mv[LIST_0];
                l1_mv_array = dec_picture->mv[LIST_1];
                l1_refframe = 0;
                
                if (l0_rFrame >=0)
                {
                  if (!l0_rFrame  && ((!moving_block[j6][i4]) && (!listX[LIST_1 + list_offset][0]->is_long_term)))
                  {
                    dec_picture->mv  [LIST_0][j4][i4][0] = 0;
                    dec_picture->mv  [LIST_0][j4][i4][1] = 0;
                    dec_picture->ref_idx[LIST_0][j4][i4] = 0;
                  }
                  else
                  {
                    dec_picture->mv  [LIST_0][j4][i4][0] = pmvl0[0];
                    dec_picture->mv  [LIST_0][j4][i4][1] = pmvl0[1];
                    dec_picture->ref_idx[LIST_0][j4][i4] = l0_rFrame;
                  }
                }
                else
                {
                  dec_picture->ref_idx[LIST_0][j4][i4] = -1;
                  dec_picture->mv  [LIST_0][j4][i4][0] = 0;
                  dec_picture->mv  [LIST_0][j4][i4][1] = 0;
                }
                
                if (l1_rFrame >=0)
                {
                  if  (l1_rFrame==0 && ((!moving_block[j6][i4]) && (!listX[LIST_1 + list_offset][0]->is_long_term)))
                  {
                    
                    dec_picture->mv  [LIST_1][j4][i4][0] = 0;
                    dec_picture->mv  [LIST_1][j4][i4][1] = 0;
                    dec_picture->ref_idx[LIST_1][j4][i4] = l1_rFrame;
                    
                  }
                  else
                  {
                    dec_picture->mv  [LIST_1][j4][i4][0] = pmvl1[0];
                    dec_picture->mv  [LIST_1][j4][i4][1] = pmvl1[1];
                    dec_picture->ref_idx[LIST_1][j4][i4] = l1_rFrame;
                  }
                }
                else
                {
                  dec_picture->mv  [LIST_1][j4][i4][0] = 0;
                  dec_picture->mv  [LIST_1][j4][i4][1] = 0;
                  dec_picture->ref_idx[LIST_1][j4][i4] = -1;
                }
                
                if (l0_rFrame < 0 && l1_rFrame < 0)
                {
                  dec_picture->ref_idx[LIST_0][j4][i4] = 0;
                  dec_picture->ref_idx[LIST_1][j4][i4] = 0;
                }
                
                l0_refframe = (dec_picture->ref_idx[LIST_0][j4][i4]!=-1) ? dec_picture->ref_idx[LIST_0][j4][i4] : 0;
                l1_refframe = (dec_picture->ref_idx[LIST_1][j4][i4]!=-1) ? dec_picture->ref_idx[LIST_1][j4][i4] : 0;
                
                l0_ref_idx = l0_refframe;
                l1_ref_idx = l1_refframe;
                
                if      (dec_picture->ref_idx[LIST_1][j4][i4]==-1) 
                {
                  direct_pdir = 0;
                  l0_refframe = ref_idx  = (dec_picture->ref_idx[LIST_0][j4][i4] != -1) ? dec_picture->ref_idx[LIST_0][j4][i4] : 0;
                  mv_array = dec_picture->mv[LIST_0];
                }
                else if (dec_picture->ref_idx[LIST_0][j4][i4]==-1) 
                {
                  direct_pdir = 1;
                  l0_refframe = ref_idx  = (dec_picture->ref_idx[LIST_1][j4][i4] != -1) ? dec_picture->ref_idx[LIST_1][j4][i4] : 0;
                  mv_array = dec_picture->mv[LIST_1];
                }
                else                                               
                  direct_pdir = 2;
                
                pred_dir = direct_pdir;
                
              }
              else
              {
                int j6= img->block_y_aff + j;
                
                int refList = (co_located_ref_idx[LIST_0][j6][i4]== -1 ? LIST_1 : LIST_0);
                int ref_idx =  co_located_ref_idx[refList][j6][i4];
                l0_mv_array = dec_picture->mv[LIST_0];
                l1_mv_array = dec_picture->mv[LIST_1];
                l1_refframe = 0;
                
                if(ref_idx==-1) // co-located is intra mode
                {
                  for(hv=0; hv<2; hv++)
                  {
                    dec_picture->mv  [LIST_0][j4][i4][hv]=0;
                    dec_picture->mv  [LIST_1][j4][i4][hv]=0;
                  }
                  
                  dec_picture->ref_idx[LIST_0][j4][i4] = 0;
                  dec_picture->ref_idx[LIST_1][j4][i4] = 0;
                  
                  l0_refframe = 0;
                  l0_ref_idx = 0;
                }
                else // co-located skip or inter mode
                {
                  int mapped_idx=0;
                  int iref;
                  
                  {
                    for (iref=0;iref<imin(img->num_ref_idx_l0_active,listXsize[LIST_0 + list_offset]);iref++)
                    {
                      if(img->structure==0 && curr_mb_field==0)
                      {
                        // If the current MB is a frame MB and the colocated is from a field picture,
                        // then the co_located_ref_id may have been generated from the wrong value of
                        // frame_poc if it references it's complementary field, so test both POC values
                        if(listX[0][iref]->top_poc*2 == co_located_ref_id[refList][j6][i4] || listX[0][iref]->bottom_poc*2 == co_located_ref_id[refList][j6][i4])
                        {
                          mapped_idx=iref;
                          break;
                        }
                        else //! invalid index. Default to zero even though this case should not happen
                          mapped_idx=INVALIDINDEX;
                        continue;
                      }
                      
                      if (dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][iref]==co_located_ref_id[refList][j6][i4])
                      {
                        mapped_idx=iref;
                        break;
                      }
                      else //! invalid index. Default to zero even though this case should not happen
                      {
                        mapped_idx=INVALIDINDEX;
                      }
                    }
                    if (INVALIDINDEX == mapped_idx)
                    {
                      error("temporal direct error\ncolocated block has ref that is unavailable",-1111);
                    }
                  }
                  
                  l0_ref_idx = mapped_idx;
                  mv_scale = img->mvscale[LIST_0 + list_offset][mapped_idx];
                  
                  //! In such case, an array is needed for each different reference.
                  if (mv_scale == 9999 || listX[LIST_0+list_offset][mapped_idx]->is_long_term)
                  {
                    dec_picture->mv  [LIST_0][j4][i4][0]=co_located_mv[refList][j6][i4][0];
                    dec_picture->mv  [LIST_0][j4][i4][1]=co_located_mv[refList][j6][i4][1];
                    
                    dec_picture->mv  [LIST_1][j4][i4][0]=0;
                    dec_picture->mv  [LIST_1][j4][i4][1]=0;
                  }
                  else
                  {
                    dec_picture->mv  [LIST_0][j4][i4][0]=(mv_scale * co_located_mv[refList][j6][i4][0] + 128 ) >> 8;
                    dec_picture->mv  [LIST_0][j4][i4][1]=(mv_scale * co_located_mv[refList][j6][i4][1] + 128 ) >> 8;
                    
                    dec_picture->mv  [LIST_1][j4][i4][0]=dec_picture->mv[LIST_0][j4][i4][0] - co_located_mv[refList][j6][i4][0] ;
                    dec_picture->mv  [LIST_1][j4][i4][1]=dec_picture->mv[LIST_0][j4][i4][1] - co_located_mv[refList][j6][i4][1] ;
                  }
                  
                  l0_refframe = dec_picture->ref_idx[LIST_0][j4][i4] = mapped_idx; //listX[1][0]->ref_idx[refList][j4][i4];
                  l1_refframe = dec_picture->ref_idx[LIST_1][j4][i4] = 0;
                  
                  l0_ref_idx = l0_refframe;
                  l1_ref_idx = l1_refframe;
                }
              }
              // store reference picture ID determined by direct mode
              dec_picture->ref_pic_id[LIST_0][j4][i4] = dec_picture->ref_pic_num[img->current_slice_nr][LIST_0 + list_offset][(short)dec_picture->ref_idx[LIST_0][j4][i4]];
              dec_picture->ref_pic_id[LIST_1][j4][i4] = dec_picture->ref_pic_num[img->current_slice_nr][LIST_1 + list_offset][(short)dec_picture->ref_idx[LIST_1][j4][i4]];
            }
            
            for (k = k_start; k < k_end; k ++)
            {
              
              i =  (decode_block_scan[k] & 3);
              j = ((decode_block_scan[k] >> 2) & 3);

              perform_mc(currMB, (ColorPlane) (uv + 1), dec_picture, img, pred_dir, i, j, list_offset, block_size_x, block_size_y, curr_mb_field);
            }
          }
        }
        iTransform(currMB, (ColorPlane) (uv + 1), img, need_4x4_transform, smb, yuv);
      }
    } 
  }  
  return 0;
} 


/*!
 ************************************************************************
 * \brief
 *    change target plane
 *    for 4:4:4 Independent mode
 ************************************************************************
 */
void change_plane_JV( int nplane )
{
    img->colour_plane_id = nplane;
    img->mb_data = img->mb_data_JV[nplane];
    dec_picture = dec_picture_JV[nplane];
    Co_located = Co_located_JV[nplane];
}

/*!
 ************************************************************************
 * \brief
 *    make frame picture from each plane data
 *    for 4:4:4 Independent mode
 ************************************************************************
 */
void make_frame_picture_JV()
{
  int uv, line;
  int nsize;
  int nplane;
  dec_picture = dec_picture_JV[0];

  // Copy Storable Params
  for( nplane=0; nplane<MAX_PLANE; nplane++ )
  {
    copy_storable_param_JV( nplane, dec_picture, dec_picture_JV[nplane] );
  }

  for( uv=0; uv<2; uv++ ){
    for( line=0; line<img->height; line++ )
    {
      nsize = sizeof(imgpel) * img->width;
      memcpy( dec_picture->imgUV[uv][line], dec_picture_JV[uv+1]->imgY[line], nsize );
    }
    free_storable_picture(dec_picture_JV[uv+1]);
  }
}


