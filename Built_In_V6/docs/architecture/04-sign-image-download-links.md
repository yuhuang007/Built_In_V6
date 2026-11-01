# 手语与手势图片数据集下载清单

日期：2026年10月11日。用户要求查国内外平台，提供链接自行下载。本次只读公开页面、作者README及平台搜索/文件元数据，未下载图片包、训练或改固件。大小为页面/API标示的近似量，压缩/解压或版本口径可能不同；下载登录要求和实际文件传输未测试。

## 建议先下载

| 数据集链接 | 已核实内容 | 许可与用途 |
| --- | --- | --- |
| [魔搭 ASL字母检测图片](https://modelscope.cn/datasets/atalaydenknalbant/asl-dataset) | README说明ASL字母、排除J/Z，YOLO txt标注；文件树确认JPG，存储约84MB | 平台标Apache 2.0，原始素材授权仍需保留核查。适合小规模图片/检测流程；不是中文词语 |
| [Kaggle ASL Alphabet](https://www.kaggle.com/datasets/grassknoted/asl-alphabet) | 87000训练图片，200×200，29类：A–Z及space/delete/nothing；自带测试仅29张，下载约1.10GB | 平台标GPL 2，采用前核实适用条款。J/Z需要运动，静态图片不能验证其完整表达 |
| [魔搭 kaggle-ASL](https://modelscope.cn/datasets/yangwolves/kaggle-ASL) | 文件树确认ASLDataSet.zip，1100887034 bytes；卡片为默认模板，未解压核实内容 | 平台标Apache 2.0，名称/大小与上述Kaggle候选吻合，但未比对文件。不能当成新增独立数据，也不能据镜像标签消除原始许可限制 |
| [Hugging Face ASL-HG](https://huggingface.co/datasets/juanjodurillo/asl-hg) | 图片Parquet，36类（0–9/A–Z），训练28800、测试7200，共36000；卡片标下载约375MB | 卡片CC BY 4.0，说明重排自[Mendeley原始记录](https://doi.org/10.17632/j4y5w2c8w9.1)。原始页面本次403，许可链未独立确认；J/Z仍有动态限制 |
| [Kaggle ASL Alphabet Test](https://www.kaggle.com/datasets/danrasband/asl-alphabet-test) | 870图片，每类30张，29类，200×200；背景有变化，搜索接口约25MB | 平台CC0，适合作外部样本检查，但人物是否独立仍需核实，不能预先宣称独立盲测 |
| [Hugging Face HaGRID小样本](https://huggingface.co/datasets/cj-mills/hagrid-sample-30k-384p) | 31833图片、384p，普通手势及no_gesture，含框和匿名user_id；来源链接[原作者仓库](https://github.com/hukenovs/hagrid) | CC BY-SA 4.0。用于手部检测、普通手势和负例；不将like/stop等标签改名为中文国家通用手语词义。无需先下载全量约1.5TB的HaGRIDv2 |

## 其他图片候选

| 链接 | 内容/边界 |
| --- | --- |
| [Kaggle Sign Language MNIST](https://www.kaggle.com/datasets/datamunge/sign-language-mnist) | 24个ASL静态字母，排除J/Z；训练27455、测试7172，28×28灰度像素CSV，搜索接口压缩约66MB；CC0。由1704原图大量增强生成，样本数不等于独立示范数，适合教程验证，不作产品跨人效果证据 |
| [Kaggle ayuraj/asl-dataset](https://www.kaggle.com/datasets/ayuraj/asl-dataset) | 文件列表确认JPEG，平台CC0；统计接口约32–60MB。说明很短，类别/人物数量本次未完整核实，不采信作者描述中的接近98%准确率作为本项目结果 |
| [Kaggle阿拉伯字母RGB图片](https://www.kaggle.com/datasets/muhammadalbrham/rgb-arabic-alphabets-sign-language-dataset) | 搜索接口约5.08GB、CC BY-SA 4.0；仅核对到检索元数据，样本及规模待审核；语言不符合中文目标 |
| [Kaggle Leap手势](https://www.kaggle.com/datasets/gti-upm/leapgestrecog) | 约2.29GB、CC BY-NC-SA 4.0，Leap Motion采集；需核对红外/灰度与设备RGB差异，不列首选 |
| [HF ASL字母10873图片](https://huggingface.co/datasets/Marxulia/asl_sign_languages_alphabets_v03) | 卡片确认26类、10873张、Parquet约74MB，但无明确许可字段，先核实授权，不因公开托管直接认定可训练 |

## 国内中文资源与平台检索边界

中文词语目标优先保留[CNSL-DP实验室](https://lise.lsnu.edu.cn/kxyj/dbxcg2.htm)及[Figshare派生帧入口](https://figshare.com/articles/media/NationalCSL-DP/27261843)。论文说明公开记录含全部视频的256×256派生帧，来源为国家通用手语6707独立动作/10人/双视角；Figshare本次403，未核实包大小与文件。原视频有机构申请要求；完整动作中的帧不全部是稳定静态正例，需要审核后筛选。

[国家通用手语词典第三方图片](https://github.com/tuzengji/chinese-sign-language-dictionary)自述6699条目/图片，但原书版权不属于上传者，未确认原权利人许可；是插图资料，不列真实照片训练首选。

| 平台 | 本次实际检索结果 |
| --- | --- |
| [魔搭](https://modelscope.cn/datasets) | 公开Query检索核对“手语/gesture/hagrid/asl”；结果含无关语音/文本数据，依据卡片与文件树筛选上述图片项。另找到SpreadTheSign抓取的视频集，与图片任务不符且上游授权未核实，不列图片下载首选 |
| [飞桨AI Studio](https://aistudio.baidu.com/datasetoverview) | 公开首页可读，但动态检索未取得可信具体条目；外部百度检索触发安全验证，不绕过。未证实有或没有适合的数据集；建议人工搜索“手语、手势、ASL、Sign Language MNIST” |
| [阿里云天池](https://tianchi.aliyun.com/dataset) | 首页可读，按公开前端接口进行手语/手势/ASL检索均403；未取得可信具体条目，保留人工检索入口，不能声明搜索完成或没有数据 |
| [Kaggle](https://www.kaggle.com/datasets) | 检索ASL alphabet、sign language mnist、Chinese sign language、American sign language、Arabic sign language及Leap；排除不相关中文结果，回读主要候选描述和许可 |
| [Hugging Face](https://huggingface.co/datasets) | 检索ASL alphabet、sign language image、HaGRID、ArSL等，回读ASL-HG/HaGRID与字母候选卡片；缺许可的不列首选 |

不是国内外所有网站的穷尽清单；搜索引擎部分结果为验证页或与关键词不符的结果，均不作为数据集证据。用户可从以上已核实条目下载，平台登录/下载可用性由实际操作确认。

## 用于产品前的边界

国外ASL字母、数字和普通手势用于图像训练、量化与推理流程验证，不构成国家通用手语词级识别完成。不能把国外字母/手势中文改名为“你好/谢谢”等词。产品训练需经规范核实的真实照片、多人覆盖、负例、按人物/源视频隔离以及目标相机补充数据；同视频抽帧或增强图片继承原集合。模型是否能部署P4需另测算子、Flash/RAM、延迟与完整集成资源。

本轮只输出下载清单，不下载数据包、不安装训练依赖、不训练、不构建、不烧录、不打开COM20；板上仍为8bf0d8e。
