Shader "HearSay/Silhouette Outline"
{
    Properties
    {
        _MainTex ("Main Texture", 2D) = "white" {}
        _Width ("Mask Width", Int) = 0
        _Height ("Mask Height", Int) = 0
        _Threshold ("Threshold", Range(0, 1)) = 0.6
    }

    SubShader
    {
        Tags { "Queue"="Transparent" "RenderType"="Transparent" }
        ZWrite Off
        Cull Off
        Blend SrcAlpha OneMinusSrcAlpha

        Pass
        {
            CGPROGRAM
            #pragma target 4.5
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            struct appdata
            {
                float4 vertex : POSITION;
                float2 uv : TEXCOORD0;
            };

            struct v2f
            {
                float2 uv : TEXCOORD0;
                float4 vertex : SV_POSITION;
            };

            int _Width;
            int _Height;
            float _Threshold;
            StructuredBuffer<float> _MaskBuffer;

            v2f vert(appdata v)
            {
                v2f o;
                o.vertex = UnityObjectToClipPos(v.vertex);
                o.uv = v.uv;
                return o;
            }

            float GetMask(float2 uv)
            {
                uv = saturate(uv);
                int x = min((int)(uv.x * _Width), _Width - 1);
                int y = min((int)(uv.y * _Height), _Height - 1);
                return _MaskBuffer[y * _Width + x];
            }

            fixed4 frag(v2f i) : SV_Target
            {
                float2 stepSize = float2(5.0 / _Width, 5.0 / _Height);
                float center = step(_Threshold, GetMask(i.uv));
                float nearby = max(
                    max(GetMask(i.uv + float2(stepSize.x, 0)),
                        GetMask(i.uv - float2(stepSize.x, 0))),
                    max(GetMask(i.uv + float2(0, stepSize.y)),
                        GetMask(i.uv - float2(0, stepSize.y)))
                );
                float edge = step(_Threshold, nearby) * (1.0 - center);
                clip(edge - 0.5);
                return fixed4(1.0, 0.88, 0.0, 1.0);
            }
            ENDCG
        }
    }
}
