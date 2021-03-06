#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ros/ros.h>

#include "cameraParameters.h"
#include "pointDefinition.h"

using namespace std;
using namespace cv;

// 相机坐标系定义：x:左; y:上; z:前，图像像素: x:右为正; y下为正

bool systemInited = false;
double timeCur, timeLast;

const int imagePixelNum = imageHeight * imageWidth;
CvSize imgSize = cvSize(imageWidth, imageHeight);

IplImage *imageCur = cvCreateImage(imgSize, IPL_DEPTH_8U, 1);
IplImage *imageLast = cvCreateImage(imgSize, IPL_DEPTH_8U, 1);

int showCount = 0;
const int showSkipNum = 2;
const int showDSRate = 2; // imshow的图像缩小的尺寸倍数
CvSize showSize = cvSize(imageWidth / showDSRate, imageHeight / showDSRate);

IplImage *imageShow = cvCreateImage(showSize, IPL_DEPTH_8U, 1);
IplImage *harrisLast = cvCreateImage(showSize, IPL_DEPTH_32F, 1);

CvMat kMat = cvMat(3, 3, CV_64FC1, kImage);
CvMat dMat = cvMat(4, 1, CV_64FC1, dImage);

IplImage *mapx, *mapy;
const int maxFeatureNumPerSubregion = 2; // 每个Subregion的的特征点个数

// Subregion x y维度数量
const int xSubregionNum = 12;
const int ySubregionNum = 8;
const int totalSubregionNum = xSubregionNum * ySubregionNum;
const int MAXFEATURENUM = maxFeatureNumPerSubregion * totalSubregionNum; // 最大特征点数量

// 边界宽度不参与到subregion
const int xBoundary = 20;
const int yBoundary = 20;

// 单个subregion 宽度和高度
const double subregionWidth = (double)(imageWidth - 2 * xBoundary) / (double)xSubregionNum;
const double subregionHeight = (double)(imageHeight - 2 * yBoundary) / (double)ySubregionNum;

const double maxTrackDis = 100;
const int winSize = 15;

IplImage *imageEig, *imageTmp, *pyrCur, *pyrLast;

// 存放特征点，CvPoint2D32f表示2通道的点（表达位置，不包含点通道描述）
CvPoint2D32f *featuresCur = new CvPoint2D32f[2 * MAXFEATURENUM];
CvPoint2D32f *featuresLast = new CvPoint2D32f[2 * MAXFEATURENUM];
char featuresFound[2 * MAXFEATURENUM];
float featuresError[2 * MAXFEATURENUM];

int featuresIndFromStart = 0;                         // 特征点从开始位置的索引，用于辅助后面对特征点计数辅助生成featuresInd
int featuresInd[2 * MAXFEATURENUM] = {0};             // 特征点索引
int totalFeatureNum = 0;                              // 总共的特征点数量
int subregionFeatureNum[2 * totalSubregionNum] = {0}; // subregion特征数量，两倍的totalSubregionNum

// 存放但前的imagePoint和上一帧的imagepoint
pcl::PointCloud<ImagePoint>::Ptr imagePointsCur(new pcl::PointCloud<ImagePoint>());
pcl::PointCloud<ImagePoint>::Ptr imagePointsLast(new pcl::PointCloud<ImagePoint>());

ros::Publisher *imagePointsLastPubPointer;
ros::Publisher *imageShowPubPointer;
cv_bridge::CvImage bridge;

// 每接收到一帧图像，则将上一帧处理的图像及特征点存为last，然后分块对image_last提取特征点，并利用LK找到imagecur相对于last匹配的特征点。
// 所有与last匹配上的特征点均赋予特征点id，用于在viodom中查找t匹配特征点。
void imageDataHandler(const sensor_msgs::Image::ConstPtr &imageData)
{
  timeLast = timeCur;                                 // 将当前时间设置为上一帧时间，timeCur的初始值为0
  timeCur = imageData->header.stamp.toSec() - 0.1163; // 从imageData获取时间戳，减掉0.1163秒？

  // imageCur变imageLast，imageLast变imagecur 相互交换，将上一次imageCur保存为imageLast
  IplImage *imageTemp = imageLast;
  imageLast = imageCur;
  imageCur = imageTemp; // 交换是防止后面修改imageCur也会修改到imageLast

  // imageCur-保存imagedata
  for (int i = 0; i < imagePixelNum; i++)
  {
    imageCur->imageData[i] = (char)imageData->data[i];
  }

  IplImage *t = cvCloneImage(imageCur); // 克隆当前帧图像，用于后续去畸变
  cvRemap(t, imageCur, mapx, mapy);     // 对当前图像重映射去畸变
  //cvEqualizeHist(imageCur, imageCur);
  cvReleaseImage(&t);             // 释放掉临时图像
  cvResize(imageLast, imageShow); // 将上一帧图像大小设置为显示的大小744*480 ->387*240

  // 猜测是提取角点，用于后边区域提取j的角点，判断角点的可靠性
  // 提取Harris角点
  /*
    参数1：原始图像，必须是灰度；
    参数2：操作后返回的图像；
    参数3：blockSize特征值计算矩阵的维数，一般是2；
    参数4：ksize平移扫过图像的矩阵块；
    参数5：表示对图像边缘的处理，这里直接BORDER_DEFAULT表示默认；
    参考：http://www.opencv.org.cn/opencvdoc/2.3.2/html/doc/tutorials/features2d/trackingmotion/harris_detector/harris_detector.html
  */
  cvCornerHarris(imageShow, harrisLast, 3);

  // featuresLast，存放检测的角点特征
  CvPoint2D32f *featuresTemp = featuresLast;
  featuresLast = featuresCur;
  featuresCur = featuresTemp;

  // 交换imagePointsLast与imagePointsCur，并清除imagePointsCur
  pcl::PointCloud<ImagePoint>::Ptr imagePointsTemp = imagePointsLast;
  imagePointsLast = imagePointsCur;
  imagePointsCur = imagePointsTemp;
  imagePointsCur->clear();

  // 保证系统初始化并得到Last帧
  if (!systemInited)
  {
    systemInited = true;
    return;
  }

  int recordFeatureNum = totalFeatureNum; // 记录特帧点数量，初始值为0
  for (int i = 0; i < ySubregionNum; i++)
  {
    for (int j = 0; j < xSubregionNum; j++)
    {
      int ind = xSubregionNum * i + j;                                      // ind为subreion在数组中的索引
      int numToFind = maxFeatureNumPerSubregion - subregionFeatureNum[ind]; // 初始为2-0
      // 个数大于0
      if (numToFind > 0)
      {
        int subregionLeft = xBoundary + (int)(subregionWidth * j);                                         // 框选出subregion，这里考虑到边界不加入subregion，subregion左边x坐标
        int subregionTop = yBoundary + (int)(subregionHeight * i);                                         // subregion上边y坐标
        CvRect subregion = cvRect(subregionLeft, subregionTop, (int)subregionWidth, (int)subregionHeight); // 在原图像上剪出subregion
                                                                                                           // cvSetImageROI改变imageLast的指针对象结构，将其指向subregion指定的图像区域，但原始图像仍旧保留，cvResetImageROI(imageLast)会将imageLast恢复为原始图像
        cvSetImageROI(imageLast, subregion);

        // subregion特征点提取，将每次提取的点云追加存放在(featuresLast)
        // featuresLast为CvPoint2D32f类型，featuresLast + totalFeatureNum索引到featuresLast上的位置
        /*
          函数 cvGoodFeaturesToTrack 在图像中寻找具有大特征值的角点。该函数，首先用cvCornerMinEigenVal 计算输入图像的每一个像素点的最小特征值，并将结果存储到变量 eig_image 中。
          然后进行非最大值抑制（仅保留3x3邻域中的局部最大值）。下一步将最小特征值小于 quality_level?max(eig_image(x,y)) 排除掉。
          最后，函数确保所有发现的角点之间具有足够的距离，（最强的角点第一个保留，然后检查新的角点与已有角点之间的距离大于 min_distance ）。 
          void cvGoodFeaturesToTrack( 
            const CvArr* image, 
            CvArr* temp_image,
            CvPoint2D32f* corners, 
            int* corner_count,
            double quality_level, 
            double min_distance,
            const CvArr* mask=NULL,
            int block_size = NULL,
            int use_harris = 0,
            double k = 0.4
          );       
          image  输入图像，8-位或浮点32-比特，单通道
          temp_image  另外一个临时图像，格式与尺寸与 eig_image 一致
          corners 输出参数，检测到的角点
          corner_count  输出参数，检测到的角点数目
          quality_level 最大最小特征值的乘法因子。定义可接受图像角点的最小质量因子。
          min_distance  限制因子。得到的角点的最小距离。使用 Euclidian 距离
          mask  ROI感兴趣区域。函数在ROI中计算角点，如果 mask 为 NULL，则选择整个图像。
          block_size  计算导数的自相关矩阵时指定点的领域，采用小窗口计算的结果比单点（也就是block_size为1）计算的结果要好。
          use_harris  标志位。当use_harris的值为非0，则函数使用Harris的角点定义；若为0，则使用Shi-Tomasi的定义。
          k 当use_harris为k且非0，则k为用于设置Hessian自相关矩阵即对Hessian行列式的相对权重的权重系数
        */
        cvGoodFeaturesToTrack(imageLast, imageEig, imageTmp, featuresLast + totalFeatureNum,
                              &numToFind, 0.1, 5.0, NULL, 3, 1, 0.04);

        int numFound = 0;
        for (int k = 0; k < numToFind; k++)
        {
          // 第k个特征点的x y加上subregionLef和subregionTop,，因为进行了ROI处理为了还原角点在源图像的坐标
          featuresLast[totalFeatureNum + k].x += subregionLeft;
          featuresLast[totalFeatureNum + k].y += subregionTop;
          // cvCornerHarris是对imageshow处理，将坐标缩小一半，加0.5 为了向上取整
          int xInd = (featuresLast[totalFeatureNum + k].x + 0.5) / showDSRate;
          int yInd = (featuresLast[totalFeatureNum + k].y + 0.5) / showDSRate;

          // 判断角点的质量，舍去一些特征点。
          // featuresLast是由上一帧curr的特征点赋值过来的，
          // (harrisLast->imageData + harrisLast->widthStep * yInd))[xInd]是[xInd,yInd]处的图像灰度值。
          // widthStep 指一行包含多少个像素。
          // 当提取的特征点对应的harris的值大于一定的值，则说明该特征点是比较可靠的角点。
          // harris的生成的角点灰度值较小，因此阈值设的很小，e-7
          if (((float *)(harrisLast->imageData + harrisLast->widthStep * yInd))[xInd] > 1e-7)
          {
            // numFound默认为0，将第numFound个角点xy设置成第k个的x,y
            featuresLast[totalFeatureNum + numFound].x = featuresLast[totalFeatureNum + k].x;
            featuresLast[totalFeatureNum + numFound].y = featuresLast[totalFeatureNum + k].y;
            // featuresIndFromStart默认为0
            featuresInd[totalFeatureNum + numFound] = featuresIndFromStart;
            // 有效角点个数加一
            numFound++;
            featuresIndFromStart++;
          }
        }
        totalFeatureNum += numFound;
        subregionFeatureNum[ind] += numFound;

        cvResetImageROI(imageLast);
      }
    }
  }

  // 使用光流法来追踪curr帧中与last帧中提取的角点相匹配的角点
  /*
    void cvCalcOpticalFlowPyrLK(const CvArr* prev, const CvArr* curr, CvArr* prev_pyr, CvArr* curr_pyr,
                                const CvPoint2D32f* prev_features, CvPoint2D32f* curr_features,
                                int count, CvSize win_size, int level, char* status,
                                float* track_error, CvTermCriteria criteria, int flags);
    prev 在时间 t 的第一帧
    curr 在时间 t + dt 的第二帧
    prev_pyr 第一帧的金字塔缓存. 如果指针非 NULL , 则缓存必须有足够的空间来存储金字塔从层 1 到层 #level 的内容。
           尺寸 (image_width+8)*image_height/3 比特足够了
    curr_pyr 与 prev_pyr 类似， 用于第二帧
    prev_features 需要发现光流的点集
    curr_features 包含新计算出来的位置的 点集
    count         特征点的数目
    win_size 每个金字塔层的搜索窗口尺寸
    level  最大的金字塔层数。如果为 0 , 不使用金字塔 (即金字塔为单层), 如果为 1 , 使用两层，下面依次类推。
    status 数组。如果对应特征的光流被发现，数组中的每一个元素都被设置为 1， 否则设置为 0。
    error  双精度数组，包含原始图像碎片与移动点之间的差。为可选参数，可以是 NULL .
    criteria 准则，指定在每个金字塔层，为某点寻找光流的迭代过程的终止条件。
    flags  其它选项：
       CV_LKFLOW_PYR_A_READY，在调用之前，第一帧的金字塔已经准备好
       CV_LKFLOW_PYR_B_READY，在调用之前，第二帧的金字塔已经准备好
       CV_LKFLOW_INITIAL_GUESSES，在调用之前，数组B包含特征的初始坐标（Hunnish: 在本节中没有出现数组B，不知是指的哪一个）
  */
  cvCalcOpticalFlowPyrLK(imageLast, imageCur, pyrLast, pyrCur,
                         featuresLast, featuresCur, totalFeatureNum, cvSize(winSize, winSize),
                         3, featuresFound, featuresError,
                         cvTermCriteria(CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 30, 0.01), 0);

  cout << "totalFeatureNum after LK:\t" << totalFeatureNum << endl;
  // 将每个subregion的特征数量标记置为0
  for (int i = 0; i < totalSubregionNum; i++)
  {
    subregionFeatureNum[i] = 0;
  }

  ImagePoint point;
  int featureCount = 0;
  double meanShiftX = 0, meanShiftY = 0;
  for (int i = 0; i < totalFeatureNum; i++)
  {
    // 判断角点投影c距离
    double trackDis = sqrt((featuresLast[i].x - featuresCur[i].x) * (featuresLast[i].x - featuresCur[i].x) + (featuresLast[i].y - featuresCur[i].y) * (featuresLast[i].y - featuresCur[i].y));
    // 判断特征点是否距离大于阈值是否越出边界
    if (!(trackDis > maxTrackDis || featuresCur[i].x < xBoundary ||
          featuresCur[i].x > imageWidth - xBoundary || featuresCur[i].y < yBoundary ||
          featuresCur[i].y > imageHeight - yBoundary))
    {
      // xInd是subregion的x方向上的索引，第几列
      // yInd是subregion在y方向上的索引，第几行
      // ind是subregion整个的索引，因此是xSubregionNum * yInd + xInd，xSubregionNum每行有几个subregion。
      int xInd = (int)((featuresLast[i].x - xBoundary) / subregionWidth);
      int yInd = (int)((featuresLast[i].y - yBoundary) / subregionHeight);
      int ind = xSubregionNum * yInd + xInd;

      // 限制每帧采集的最大点数
      if (subregionFeatureNum[ind] < maxFeatureNumPerSubregion)
      {
        featuresCur[featureCount].x = featuresCur[i].x;
        featuresCur[featureCount].y = featuresCur[i].y;
        featuresLast[featureCount].x = featuresLast[i].x;
        featuresLast[featureCount].y = featuresLast[i].y;
        featuresInd[featureCount] = featuresInd[i];

        // x,y为特征点在图像上的坐标，使用相机内参矩阵K将点投影在相机坐标系下的坐标。
        // 注意这里的u v是相机下的坐标，x,y为特征点在图像上的像素坐标
        // 相机坐标系定义：x:左; y:上; z:前，图像像素: x:右为正; y下为正
        point.u = -(featuresCur[featureCount].x - kImage[2]) / kImage[0];
        point.v = -(featuresCur[featureCount].y - kImage[5]) / kImage[4];
        point.ind = featuresInd[featureCount];
        imagePointsCur->push_back(point);

        // 防止重复添加
        if (i >= recordFeatureNum)
        {
          // u=x/z，v=y/z，得到的是像素的归一化后的相机坐标系下的三维坐标
          // 对图像点进行归一化，即将图像点都归一化到深度为1的平面上
          // 在visual odom 计算时，会将雷达深度点云及图像点云放在深度为10的平面上。
          point.u = -(featuresLast[featureCount].x - kImage[2]) / kImage[0];
          point.v = -(featuresLast[featureCount].y - kImage[5]) / kImage[4];
          imagePointsLast->push_back(point);
        }

        // 匹配特征点在图像上的偏移量
        meanShiftX += fabs((featuresCur[featureCount].x - featuresLast[featureCount].x) / kImage[0]);
        meanShiftY += fabs((featuresCur[featureCount].y - featuresLast[featureCount].y) / kImage[4]);

        featureCount++;
        // 统计每个subregion的特征点数量，保证每个subregion的特征点不多于两个
        subregionFeatureNum[ind]++;
      }
    }
  }

  totalFeatureNum = featureCount;
  cout << "feature tracking: totalFeatureNum:\t" << totalFeatureNum << endl;

  // 匹配特征点在图像上的偏移量均值
  meanShiftX /= totalFeatureNum;
  meanShiftY /= totalFeatureNum;

  // 将imagePointsLast发出去
  sensor_msgs::PointCloud2 imagePointsLast2;

  cout << "PointsLast num total:\t" << imagePointsLast->points.size() << endl;

  pcl::toROSMsg(*imagePointsLast, imagePointsLast2);
  imagePointsLast2.header.stamp = ros::Time().fromSec(timeLast);
  //imagePointsLast2.header.frame_id = "/camera_init";
  //imagePointsLast2.header.frame_id = "/camera";

  imagePointsLastPubPointer->publish(imagePointsLast2); // 发布特帧点
  showCount = (showCount + 1) % (showSkipNum + 1);      // 发布要显示的图像，两帧发布一次
  if (showCount == showSkipNum)
  {
    // Mat imageShowMat(imageShow);
    // imageShow中存放使用cvCornerHarris找到的角点图
    Mat imageShowMat(cv::cvarrToMat(imageShow));
    bridge.image = imageShowMat;
    bridge.encoding = "mono8"; // mono8格式呈现的效果是存储下来的图像为单色。一般用于灰度图的呈现。
    sensor_msgs::Image::Ptr imageShowPointer = bridge.toImageMsg();
    imageShowPubPointer->publish(imageShowPointer);
  }
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "featureTracking");
  ros::NodeHandle nh;

  // 在校准参数的基础上创建映射矩阵
  mapx = cvCreateImage(imgSize, IPL_DEPTH_32F, 1);
  mapy = cvCreateImage(imgSize, IPL_DEPTH_32F, 1);

  // 根据相机的参数矩阵及畸变参数，与cvRemap结合来标定及校正图像
  cvInitUndistortMap(&kMat, &dMat, mapx, mapy);

  CvSize subregionSize = cvSize((int)subregionWidth, (int)subregionHeight);
  imageEig = cvCreateImage(subregionSize, IPL_DEPTH_32F, 1);
  imageTmp = cvCreateImage(subregionSize, IPL_DEPTH_32F, 1);

  CvSize pyrSize = cvSize(imageWidth + 8, imageHeight / 3); // ???
  pyrCur = cvCreateImage(pyrSize, IPL_DEPTH_32F, 1);
  pyrLast = cvCreateImage(pyrSize, IPL_DEPTH_32F, 1);

  // 接收/image/raw，在imageDatqaHandler函数处理图片
  ros::Subscriber imageDataSub = nh.subscribe<sensor_msgs::Image>("/image/raw", 1, imageDataHandler);

  ros::Publisher imagePointsLastPub = nh.advertise<sensor_msgs::PointCloud2>("/image_points_last", 5);
  // 发布上一帧特征点归一化图像坐标
  imagePointsLastPubPointer = &imagePointsLastPub;

  ros::Publisher imageShowPub = nh.advertise<sensor_msgs::Image>("/image/show", 1); // 发布展示的点云图像
  imageShowPubPointer = &imageShowPub;

  ros::spin();

  return 0;
}
