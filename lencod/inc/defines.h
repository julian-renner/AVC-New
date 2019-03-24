/*
***********************************************************************
* COPYRIGHT AND WARRANTY INFORMATION
*
* Copyright 2001, International Telecommunications Union, Geneva
*
* DISCLAIMER OF WARRANTY
*
* These software programs are available to the user without any
* license fee or royalty on an "as is" basis. The ITU disclaims
* any and all warranties, whether express, implied, or
* statutory, including any implied warranties of merchantability
* or of fitness for a particular purpose.  In no event shall the
* contributor or the ITU be liable for any incidental, punitive, or
* consequential damages of any kind whatsoever arising from the
* use of these programs.
*
* This disclaimer of warranty extends to the user of these programs
* and user's customers, employees, agents, transferees, successors,
* and assigns.
*
* The ITU does not represent or warrant that the programs furnished
* hereunder are free of infringement of any third-party patents.
* Commercial implementations of ITU-T Recommendations, including
* shareware, may be subject to royalty fees to patent holders.
* Information regarding the ITU-T patent policy is available from
* the ITU Web site at http://www.itu.int.
*
* THIS IS NOT A GRANT OF PATENT RIGHTS - SEE THE ITU-T PATENT POLICY.
************************************************************************
*/

/*!
 **************************************************************************
 * \file defines.h
 *
 * \brief
 *    Headerfile containing some useful global definitions
 *
 * \author
 *    Detlev Marpe 
 *    Copyright (C) 2000 HEINRICH HERTZ INSTITUTE All Rights Reserved.
 *
 * \date
 *    21. March 2001
 **************************************************************************
 */


#ifndef _DEFINES_H_
#define _DEFINES_H_


#define _EXP_GOLOMB
//#define USE_6_INTRA_MODES


// Constants for the interim file format
#define WORKING_DRAFT_MAJOR_NO 0    // inidicate the working draft version number
#define WORKING_DRAFT_MINOR_NO 4
#define INTERIM_FILE_MAJOR_NO 0     // indicate interim file format version number
#define INTERIM_FILE_MINOR_NO 1

#define _FAST_FULL_ME_
#define _FULL_SEARCH_RANGE_
#define _ADAPT_LAST_GROUP_
#define _CHANGE_QP_
#define _ADDITIONAL_REFERENCE_FRAME_
#define _LEAKYBUCKET_

// #define _CHECK_MULTI_BUFFER_1_
// #define _CHECK_MULTI_BUFFER_2_

// #define USE_6_INTRA_MODES

#define IMG_PAD_SIZE    4   //!< Number of pixels padded around the reference frame (>=4)

#define TRACE           1        //!< 0:Trace off 1:Trace on

#define absm(A) ((A)<(0) ? (-(A)):(A)) //!< abs macro, faster than procedure
#define MAX_VALUE       999999   //!< used for start value for some variables





#define P8x8    8
#define I4MB    9
#define I16MB   10
#define IBLOCK  11
#define MAXMODE 12


#define  LAMBDA_ACCURACY_BITS         16
#define  LAMBDA_FACTOR(lambda)        ((int)((double)(1<<LAMBDA_ACCURACY_BITS)*lambda+0.5))
#define  WEIGHTED_COST(factor,bits)   (((factor)*(bits))>>LAMBDA_ACCURACY_BITS)
#define  MV_COST(f,s,cx,cy,px,py)     (WEIGHTED_COST(f,mvbits[((cx)<<(s))-px]+mvbits[((cy)<<(s))-py]))
#define  REF_COST(f,ref)              (WEIGHTED_COST(f,refbits[(ref)]))


#define IS_INTRA(MB)    ((MB)->mb_type==I4MB  || (MB)->mb_type==I16MB)
#define IS_NEWINTRA(MB) ((MB)->mb_type==I16MB)
#define IS_OLDINTRA(MB) ((MB)->mb_type==I4MB)
#define IS_INTER(MB)    ((MB)->mb_type!=I4MB  && (MB)->mb_type!=I16MB)
#define IS_INTERMV(MB)  ((MB)->mb_type!=I4MB  && (MB)->mb_type!=I16MB  && (MB)->mb_type!=0)
#define IS_DIRECT(MB)   ((MB)->mb_type==0     && img ->   type==B_IMG)
#define IS_COPY(MB)     ((MB)->mb_type==0     && img ->   type==INTER_IMG);
#define IS_P8x8(MB)     ((MB)->mb_type==P8x8)





// Quantization parameter range
#define MIN_QP          -8
#define MAX_QP          39

// Picture types
#define INTRA_IMG       0   //!< I frame
#define INTER_IMG       1   //!< P frame
#define B_IMG           2   //!< B frame
#define SP_IMG          3   //!< SP frame


#define BLOCK_SIZE      4
#define MB_BLOCK_SIZE   16

#ifndef USE_6_INTRA_MODES

#define NO_INTRA_PMODE  9        //!< #intra prediction modes
//!< 4x4 intra prediction modes
#define DC_PRED         0
#define VERT_PRED       1
#define HOR_PRED        2
#define DIAG_PRED_SE    3
#define DIAG_PRED_NE    4
#define DIAG_PRED_SSE   5
#define DIAG_PRED_NNE   6
#define DIAG_PRED_ENE   7
#define DIAG_PRED_ESE   8

#else //!< USE_6_INTRA_MODES

#define NO_INTRA_PMODE  6        //!< #intra prediction modes
//!< 4x4 intra prediction modes
#define DC_PRED         0
#define DIAG_PRED_RL    1
#define VERT_PRED       2
#define DIAG_PRED_LR_45 3
#define HOR_PRED        4
#define DIAG_PRED_LR    5

#endif //!< USE_6_INTRA_MODES

// 16x16 intra prediction modes
#define VERT_PRED_16    0
#define HOR_PRED_16     1
#define DC_PRED_16      2
#define PLANE_16        3

// image formats
#define SUB_QCIF        0       // GH added picture formats
#define QCIF            1
#define CIF             2
#define CIF_4           3       // GH added picture formats
#define CIF_16          4       // GH added picture formats
#define CUSTOM          5       // GH added picture formats

// QCIF format
#define IMG_WIDTH       176
#define IMG_HEIGHT      144
#define IMG_WIDTH_CR    88
#define IMG_HEIGHT_CR   72

#define INIT_FRAME_RATE 30
#define LEN_STARTCODE   31        //!< length of start code
#define EOS             1         //!< End Of Sequence


#define MVPRED_MEDIAN   0
#define MVPRED_L        1
#define MVPRED_U        2
#define MVPRED_UR       3


#define BLOCK_MULTIPLE      (MB_BLOCK_SIZE/BLOCK_SIZE)

#define MAX_SYMBOLS_PER_MB  600 //!< Maximum number of different syntax elements for one MB

#define MAX_PART_NR     8 /*!< Maximum number of different data partitions.
                               Some reasonable number which should reflect
                               what is currently defined in the SE2Partition map (elements.h) */

#define MAXPICTURETYPESEQUENCELEN 100   /*!< Maximum size of the string that defines the picture
                                             types to be coded, e.g. "IBBPBBPBB" */
#endif
